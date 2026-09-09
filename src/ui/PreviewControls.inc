    // The preview is intentionally kept in this include so it can use the
    // MainWindow calibration state without turning the small Win32 UI into a
    // second owner of the rectangles. The controls below are all optional:
    // the test harness supplies a plain STATIC window and exercises the same
    // image-space interaction helpers directly.

    enum class PreviewTool { CaptureArea, Scrollbar };
    using ActiveTool = PreviewTool;

    static constexpr int kPreviewToolbarCapture = 0x6601;
    static constexpr int kPreviewToolbarScrollbar = 0x6602;
    static constexpr int kPreviewToolbarDraw = 0x6603;
    static constexpr int kPreviewToolbarUndo = 0x6604;
    static constexpr int kPreviewToolbarReset = 0x6605;
    static constexpr int kPreviewToolbarFit = 0x6606;
    static constexpr int kPreviewToolbarActual = 0x6607;
    static constexpr int kPreviewToolbarPrevious = 0x6608;
    static constexpr int kPreviewToolbarNext = 0x6609;

    static constexpr int kPreviewToolbarHeight = 54;
    static constexpr int kPreviewToolbarWrapHeight = 96;
    static constexpr int kPreviewMinWidth = 720;
    static constexpr int kPreviewMinHeight = 500;
    static constexpr int kPreviewHandlePixels = 10;
    static constexpr int kPreviewThinTrackPixels = 22;
    static constexpr int kPreviewMoveGripPixels = 14;
    static constexpr double kPreviewMinimumZoom = 0.05;
    static constexpr double kPreviewMaximumZoom = 8.0;
    static constexpr std::size_t kPreviewHistoryLimit = 32;

    struct DisplayTransform {
        RECT destination{};
        double scale = 1.0;
        double originX = 0.0;
        double originY = 0.0;
        RECT content{};
    };

    struct CalibrationSnapshot {
        Rect viewport{};
        Rect track{};
        ScrollbarSide side = ScrollbarSide::Right;
        std::optional<std::size_t> candidate;
        std::vector<ScrollbarCandidate> candidates;
        bool viewportCustomized = false;
        double zoom = 0.0;
        double panX = 0.0;
        double panY = 0.0;
        bool zoomFit = true;
    };

    // Toolbar HWNDs are children of the detached preview window. They are
    // kept here instead of in main.cpp so WM_COMMAND and WM_SIZE handling is
    // local to the preview interaction state.
    HWND previewToolCaptureButton_ = nullptr;
    HWND previewToolScrollbarButton_ = nullptr;
    HWND previewDrawButton_ = nullptr;
    HWND previewUndoButton_ = nullptr;
    HWND previewResetButton_ = nullptr;
    HWND previewFitButton_ = nullptr;
    HWND previewActualButton_ = nullptr;
    HWND previewPreviousButton_ = nullptr;
    HWND previewNextButton_ = nullptr;
    HWND previewToolbarLabel_ = nullptr;
    HFONT previewBodyFont_ = nullptr;
    UINT previewDpi_ = USER_DEFAULT_SCREEN_DPI;
    int previewToolbarHeight_ = 0;
    bool previewToolbarCreated_ = false;

    PreviewTool activePreviewTool_ = PreviewTool::CaptureArea;
    bool drawingReplacement_ = false;
    bool panningPreview_ = false;
    bool spaceDown_ = false;
    bool gestureActive_ = false;
    bool gestureChanged_ = false;
    bool viewportCustomized_ = false;
    bool historyRestoreInProgress_ = false;
    CalibrationSnapshot gestureBefore_{};
    std::vector<CalibrationSnapshot> calibrationHistory_;
    POINT panStartClient_{};
    double panOriginalX_ = 0.0;
    double panOriginalY_ = 0.0;
    POINT lastImagePoint_{};
    double previewZoom_ = 0.0;
    double previewPanX_ = 0.0;
    double previewPanY_ = 0.0;
    bool previewZoomFit_ = true;

    [[nodiscard]] RECT previewContentRect(HWND canvas) const {
        RECT client{};
        if (!canvas) return client;
        GetClientRect(canvas, &client);
        const int clientHeight = static_cast<int>(client.bottom - client.top);
        const int toolbar = std::clamp(previewToolbarHeight_, 0, std::max(0, clientHeight));
        client.top += toolbar;
        return client;
    }

    [[nodiscard]] double previewFitScale(HWND canvas) const {
        if (preview_.empty()) return 1.0;
        const RECT content = previewContentRect(canvas);
        const int width = content.right - content.left;
        const int height = content.bottom - content.top;
        if (width <= 2 || height <= 2) return 1.0;
        const double availableWidth = std::max(1, width - 16);
        const double availableHeight = std::max(1, height - 16);
        return std::max(0.0001, std::min(availableWidth / preview_.cols,
                                          availableHeight / preview_.rows));
    }

    [[nodiscard]] std::optional<DisplayTransform> displayTransform(HWND canvas) const {
        if (preview_.empty() || !canvas) return std::nullopt;
        const RECT content = previewContentRect(canvas);
        const int contentWidth = content.right - content.left;
        const int contentHeight = content.bottom - content.top;
        if (contentWidth <= 2 || contentHeight <= 2) return std::nullopt;

        const double scale = previewZoomFit_
            ? previewFitScale(canvas)
            : std::clamp(previewZoom_, kPreviewMinimumZoom, kPreviewMaximumZoom);
        if (!(scale > 0.0) || !std::isfinite(scale)) return std::nullopt;

        // The pan is applied after centering the complete original capture.
        // Keeping originX/originY as doubles makes cursor-anchored zoom exact
        // even though GDI receives integer destination coordinates.
        const double centeredX = (content.left + content.right - preview_.cols * scale) / 2.0;
        const double centeredY = (content.top + content.bottom - preview_.rows * scale) / 2.0;
        const double originX = centeredX + previewPanX_;
        const double originY = centeredY + previewPanY_;
        const int right = static_cast<int>(std::lround(originX + preview_.cols * scale));
        const int bottom = static_cast<int>(std::lround(originY + preview_.rows * scale));
        return DisplayTransform{
            {static_cast<LONG>(std::lround(originX)), static_cast<LONG>(std::lround(originY)),
             static_cast<LONG>(right), static_cast<LONG>(bottom)},
            scale, originX, originY, content};
    }

    [[nodiscard]] bool clientToImage(HWND canvas, int clientX, int clientY,
                                     POINT& imagePoint) const {
        const auto transform = displayTransform(canvas);
        if (!transform) return false;
        const RECT& content = transform->content;
        if (clientX < content.left || clientX >= content.right ||
            clientY < content.top || clientY >= content.bottom) {
            return false;
        }
        imagePoint.x = std::clamp(static_cast<int>(std::floor(
                                      (static_cast<double>(clientX) - transform->originX) /
                                      transform->scale)),
                                  0, std::max(0, preview_.cols - 1));
        imagePoint.y = std::clamp(static_cast<int>(std::floor(
                                      (static_cast<double>(clientY) - transform->originY) /
                                      transform->scale)),
                                  0, std::max(0, preview_.rows - 1));
        return clientX >= transform->destination.left && clientX < transform->destination.right &&
               clientY >= transform->destination.top && clientY < transform->destination.bottom;
    }

    [[nodiscard]] POINT imageToClient(HWND canvas, double imageX, double imageY) const {
        const auto transform = displayTransform(canvas);
        if (!transform) return POINT{};
        return POINT{
            static_cast<LONG>(std::lround(transform->originX + imageX * transform->scale)),
            static_cast<LONG>(std::lround(transform->originY + imageY * transform->scale))};
    }

    static RECT scaledRect(const Rect& rect, const DisplayTransform& transform) {
        return {
            static_cast<LONG>(std::lround(transform.originX + rect.x * transform.scale)),
            static_cast<LONG>(std::lround(transform.originY + rect.y * transform.scale)),
            static_cast<LONG>(std::lround(transform.originX + rect.right() * transform.scale)),
            static_cast<LONG>(std::lround(transform.originY + rect.bottom() * transform.scale))};
    }

    [[nodiscard]] Rect constrainedPreviewRect(const Rect& requested) const {
        if (preview_.empty()) return requested;
        const int imageWidth = std::max(1, preview_.cols);
        const int imageHeight = std::max(1, preview_.rows);
        Rect clipped = requested.clampTo(imageWidth, imageHeight);
        if (clipped.width <= 0) {
            clipped.x = std::clamp(requested.x, 0, imageWidth - 1);
            clipped.width = 1;
        }
        if (clipped.height <= 0) {
            clipped.y = std::clamp(requested.y, 0, imageHeight - 1);
            clipped.height = 1;
        }
        return clipped;
    }

    // The preview treats the header as part of the green capture viewport.
    // This gives the stitcher one continuous region and avoids drawing or
    // emitting a fixed header a second time when the scrollable band moves.
    [[nodiscard]] Rect viewportForTrack(const Rect& requestedTrack) const {
        if (preview_.empty()) return {};
        const Rect track = constrainedPreviewRect(requestedTrack);
        const int imageWidth = std::max(1, preview_.cols);
        const int imageHeight = std::max(1, preview_.rows);
        if (scrollbarSide_ == ScrollbarSide::Right) {
            const int width = std::clamp(track.x, 1, imageWidth);
            const int height = std::clamp(track.bottom(), 1, imageHeight);
            return {0, 0, width, height};
        }
        const int left = std::clamp(track.right(), 0, imageWidth - 1);
        return {left, 0, std::max(1, imageWidth - left),
                std::clamp(track.bottom(), 1, imageHeight)};
    }

    void setPreviewRect(const std::array<HWND, 4>& edits, const Rect& requested) {
        setRect(edits, constrainedPreviewRect(requested));
    }

    void constrainPreviewGeometry() {
        if (preview_.empty()) return;
        setPreviewRect(viewportEdits_, readRect(viewportEdits_));
        setPreviewRect(trackEdits_, readRect(trackEdits_));
        InvalidateRect(previewCanvas_, nullptr, FALSE);
    }

    [[nodiscard]] CalibrationSnapshot captureCalibrationSnapshot() const {
        return CalibrationSnapshot{
            readRect(viewportEdits_), readRect(trackEdits_), scrollbarSide_,
            selectedScrollbarCandidate_, scrollbarCandidates_, viewportCustomized_, previewZoom_, previewPanX_,
            previewPanY_, previewZoomFit_};
    }

    static bool sameRect(const Rect& left, const Rect& right) {
        return left.x == right.x && left.y == right.y && left.width == right.width &&
               left.height == right.height;
    }

    static bool sameSnapshot(const CalibrationSnapshot& left, const CalibrationSnapshot& right) {
        const auto sameCandidates = [](const std::vector<ScrollbarCandidate>& a,
                                       const std::vector<ScrollbarCandidate>& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (!sameRect(a[i].config.track, b[i].config.track) ||
                    a[i].config.side != b[i].config.side) return false;
            }
            return true;
        };
        return sameRect(left.viewport, right.viewport) && sameRect(left.track, right.track) &&
               left.side == right.side && left.candidate == right.candidate &&
               sameCandidates(left.candidates, right.candidates) &&
               left.viewportCustomized == right.viewportCustomized &&
               left.zoom == right.zoom && left.panX == right.panX && left.panY == right.panY &&
               left.zoomFit == right.zoomFit;
    }

    void pushCalibrationHistory(const CalibrationSnapshot& snapshot) {
        if (historyRestoreInProgress_) return;
        if (!calibrationHistory_.empty() && sameSnapshot(calibrationHistory_.back(), snapshot)) return;
        calibrationHistory_.push_back(snapshot);
        if (calibrationHistory_.size() > kPreviewHistoryLimit) {
            calibrationHistory_.erase(calibrationHistory_.begin());
        }
    }

    void restoreCalibrationSnapshot(const CalibrationSnapshot& snapshot) {
        historyRestoreInProgress_ = true;
        if (!preview_.empty()) {
            setPreviewRect(viewportEdits_, snapshot.viewport);
            setPreviewRect(trackEdits_, snapshot.track);
        } else {
            setRect(viewportEdits_, snapshot.viewport);
            setRect(trackEdits_, snapshot.track);
        }
        scrollbarSide_ = snapshot.side;
        scrollbarCandidates_ = snapshot.candidates;
        selectedScrollbarCandidate_ = snapshot.candidate;
        viewportCustomized_ = snapshot.viewportCustomized;
        previewZoom_ = snapshot.zoom;
        previewPanX_ = snapshot.panX;
        previewPanY_ = snapshot.panY;
        previewZoomFit_ = snapshot.zoomFit;
        refreshScrollbarCandidateCombo();
        historyRestoreInProgress_ = false;
        updatePreviewToolButtons();
        refreshWorkflow();
        InvalidateRect(previewCanvas_, nullptr, FALSE);
    }

    // Parent-side numeric edits and templates call this before changing the
    // calibration values. It gives those changes the same bounded undo stack
    // as canvas gestures and candidate selection.
    void rememberCalibration() {
        if (historyRestoreInProgress_) return;
        pushCalibrationHistory(captureCalibrationSnapshot());
    }

    void undoPreviewCalibration() {
        if (session_.state() == SessionState::Capturing ||
            session_.state() == SessionState::Paused ||
            session_.state() == SessionState::Finalized) return;
        if (previewInteractionActive() || drawingReplacement_) {
            cancelPreviewGesture(previewCanvas_);
            return;
        }
        if (calibrationHistory_.empty()) return;
        const CalibrationSnapshot snapshot = calibrationHistory_.back();
        calibrationHistory_.pop_back();
        restoreCalibrationSnapshot(snapshot);
    }

    void undoCalibration() { undoPreviewCalibration(); }

    void beginPreviewGesture() {
        if (gestureActive_) return;
        gestureBefore_ = captureCalibrationSnapshot();
        gestureActive_ = true;
        gestureChanged_ = false;
    }

    void finishPreviewGesture(bool restore) {
        if (!gestureActive_) return;
        if (restore) {
            restoreCalibrationSnapshot(gestureBefore_);
        } else if (gestureChanged_ && !panningPreview_) {
            pushCalibrationHistory(gestureBefore_);
        }
        gestureActive_ = false;
        gestureChanged_ = false;
        panningPreview_ = false;
        drawingReplacement_ = false;
        refreshWorkflow();
        updatePreviewToolButtons();
    }

    void cancelPreviewGesture(HWND canvas = nullptr) {
        if (gestureActive_) finishPreviewGesture(true);
        else {
            drawingReplacement_ = false;
            panningPreview_ = false;
        }
        dragTarget_ = CanvasTarget::None;
        dragEdges_ = EdgeNone;
        if (canvas && GetCapture() == canvas) ReleaseCapture();
        updatePreviewToolButtons();
        if (canvas) InvalidateRect(canvas, nullptr, FALSE);
    }

    [[nodiscard]] bool previewInteractionActive() const {
        return gestureActive_ || dragTarget_ != CanvasTarget::None ||
               panningPreview_;
    }

    void resetPreviewInteraction() {
        if (previewInteractionActive()) cancelPreviewGesture(previewCanvas_);
        calibrationHistory_.clear();
        gestureActive_ = false;
        gestureChanged_ = false;
        drawingReplacement_ = false;
        panningPreview_ = false;
        dragTarget_ = CanvasTarget::None;
        dragEdges_ = EdgeNone;
        viewportCustomized_ = false;
        previewZoom_ = 0.0;
        previewPanX_ = 0.0;
        previewPanY_ = 0.0;
        previewZoomFit_ = true;
        activePreviewTool_ = PreviewTool::CaptureArea;
        spaceDown_ = false;
        updatePreviewToolButtons();
        InvalidateRect(previewCanvas_, nullptr, FALSE);
    }

    void markViewportCustomized() {
        viewportCustomized_ = true;
    }

    [[nodiscard]] bool previewCalibrationMutable() const {
        return (!window_ || calibrationReady_) && session_.state() != SessionState::Capturing &&
               session_.state() != SessionState::Paused &&
               session_.state() != SessionState::Finalized;
    }

    void setActivePreviewTool(PreviewTool tool) {
        cancelPreviewGesture(previewCanvas_);
        activePreviewTool_ = tool;
        drawingReplacement_ = false;
        updatePreviewToolButtons();
        updatePreviewCursor(previewCanvas_, nullptr);
    }

    void setPreviewTool(PreviewTool tool) { setActivePreviewTool(tool); }

    [[nodiscard]] PreviewTool activePreviewTool() const { return activePreviewTool_; }
    [[nodiscard]] PreviewTool activeTool() const { return activePreviewTool_; }

    // MainWindow's message pump can call this before IsDialogMessageW when a
    // toolbar child has focus. That keeps Escape, Ctrl+Z, and Space usable
    // without requiring every toolbar button to be subclassed.
    bool handlePreviewShortcut(MSG& message) {
        if (!previewCanvas_ ||
            GetAncestor(message.hwnd, GA_ROOT) != previewCanvas_) return false;
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            SendMessageW(previewCanvas_, WM_KEYDOWN, VK_ESCAPE, message.lParam);
            return true;
        }
        if (message.message == WM_KEYDOWN &&
            (message.wParam == 'Z' || message.wParam == 'z') &&
            (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(previewCanvas_, WM_KEYDOWN, message.wParam, message.lParam);
            return true;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_SPACE) {
            SendMessageW(previewCanvas_, WM_KEYDOWN, VK_SPACE, message.lParam);
            return true;
        }
        if (message.message == WM_KEYUP && message.wParam == VK_SPACE) {
            SendMessageW(previewCanvas_, WM_KEYUP, VK_SPACE, message.lParam);
            return true;
        }
        return false;
    }

    void startDrawReplacement() {
        if (!previewCalibrationMutable()) return;
        cancelPreviewGesture(previewCanvas_);
        drawingReplacement_ = true;
        updatePreviewToolButtons();
        updatePreviewCursor(previewCanvas_, nullptr);
        InvalidateRect(previewCanvas_, nullptr, FALSE);
    }

    void resetActivePreviewArea() {
        if (preview_.empty() || !previewCalibrationMutable()) return;
        rememberCalibration();
        if (activePreviewTool_ == PreviewTool::CaptureArea) {
            viewportCustomized_ = false;
            setPreviewRect(viewportEdits_, viewportForTrack(readRect(trackEdits_)));
        } else {
            const Rect previousTrack = readRect(trackEdits_);
            setPreviewRect(trackEdits_, previousTrack);
            if (!selectedScrollbarCandidate_ ||
                *selectedScrollbarCandidate_ >= scrollbarCandidates_.size()) {
                scrollbarSide_ = previousTrack.x + previousTrack.width / 2 < preview_.cols / 2
                    ? ScrollbarSide::Left : ScrollbarSide::Right;
            }
        }
        refreshWorkflow();
        InvalidateRect(previewCanvas_, nullptr, FALSE);
    }

    void fitPreview() {
        previewZoom_ = 0.0;
        previewPanX_ = 0.0;
        previewPanY_ = 0.0;
        previewZoomFit_ = true;
        InvalidateRect(previewCanvas_, nullptr, FALSE);
    }

    void actualSizePreview() {
        previewZoom_ = 1.0;
        previewZoomFit_ = false;
        previewPanX_ = 0.0;
        previewPanY_ = 0.0;
        InvalidateRect(previewCanvas_, nullptr, FALSE);
    }

    [[nodiscard]] double previewScale(HWND canvas) const {
        const auto transform = displayTransform(canvas);
        return transform ? transform->scale : 1.0;
    }

    void zoomPreviewAt(HWND canvas, int clientX, int clientY, int wheelDelta) {
        if (preview_.empty() || wheelDelta == 0) return;
        const auto transform = displayTransform(canvas);
        if (!transform) return;
        const RECT& content = transform->content;
        if (clientX < content.left || clientX >= content.right ||
            clientY < content.top || clientY >= content.bottom) return;

        const double imageX = (clientX - transform->originX) / transform->scale;
        const double imageY = (clientY - transform->originY) / transform->scale;
        const int notches = wheelDelta / WHEEL_DELTA;
        const int effectiveNotches = notches == 0 ? (wheelDelta > 0 ? 1 : -1) : notches;
        const double oldScale = transform->scale;
        const double newScale = std::clamp(
            previewZoomFit_ ? oldScale * std::pow(1.15, effectiveNotches)
                            : previewZoom_ * std::pow(1.15, effectiveNotches),
            kPreviewMinimumZoom, kPreviewMaximumZoom);
        const double centeredX = (content.left + content.right - preview_.cols * newScale) / 2.0;
        const double centeredY = (content.top + content.bottom - preview_.rows * newScale) / 2.0;
        previewZoomFit_ = false;
        previewZoom_ = newScale;
        previewPanX_ = clientX - imageX * newScale - centeredX;
        previewPanY_ = clientY - imageY * newScale - centeredY;
        InvalidateRect(canvas, nullptr, FALSE);
    }

    static bool hitRect(const Rect& rect, const POINT& point, int radius, int& edges) {
        if (!rect.valid()) return false;
        const bool inside = point.x >= rect.x - radius && point.x <= rect.right() + radius &&
                            point.y >= rect.y - radius && point.y <= rect.bottom() + radius;
        if (!inside) return false;
        edges = EdgeNone;
        const bool nearLeft = std::abs(point.x - rect.x) <= radius;
        const bool nearRight = std::abs(point.x - rect.right()) <= radius;
        const bool nearTop = std::abs(point.y - rect.y) <= radius;
        const bool nearBottom = std::abs(point.y - rect.bottom()) <= radius;
        // A one or two pixel track has overlapping left/right edges. Pick a
        // move hit in its central band so it remains possible to reposition.
        if (rect.width > radius * 2) {
            if (nearLeft) edges |= EdgeLeft;
            if (nearRight) edges |= EdgeRight;
        }
        if (rect.height > radius * 2) {
            if (nearTop) edges |= EdgeTop;
            if (nearBottom) edges |= EdgeBottom;
        }
        return true;
    }

    [[nodiscard]] bool trackMoveGripHit(const Rect& track, const POINT& point,
                                        double scale) const {
        if (!track.valid() || track.width * scale >= previewDpiPixels(30)) return false;
        const int gripHalf = std::max(1, static_cast<int>(std::ceil(
            (previewDpiPixels(kPreviewMoveGripPixels) / 2.0) / std::max(0.01, scale))));
        const int center = track.x + track.width / 2;
        const int middle = track.y + track.height / 2;
        return point.x >= center - gripHalf && point.x <= center + gripHalf &&
               point.y >= middle - gripHalf && point.y <= middle + gripHalf;
    }

    using PreviewHandle = std::pair<POINT, int>;
    std::vector<PreviewHandle> previewHandles(const Rect& rect, const DisplayTransform& transform) const {
        const RECT bounds = scaledRect(rect, transform);
        const int cx = (bounds.left + bounds.right) / 2, cy = (bounds.top + bounds.bottom) / 2;
        if (bounds.right - bounds.left < previewDpiPixels(30)) {
            // Separate side grips in screen space so even a one-pixel track
            // can be widened without its left and right handles overlapping.
            return {{{cx, bounds.top}, EdgeTop}, {{cx, bounds.bottom}, EdgeBottom},
                    {{cx - previewDpiPixels(18), cy}, EdgeLeft},
                    {{cx + previewDpiPixels(18), cy}, EdgeRight}};
        }
        return {{{bounds.left, bounds.top}, EdgeLeft | EdgeTop}, {{cx, bounds.top}, EdgeTop},
                {{bounds.right, bounds.top}, EdgeRight | EdgeTop}, {{bounds.right, cy}, EdgeRight},
                {{bounds.right, bounds.bottom}, EdgeRight | EdgeBottom}, {{cx, bounds.bottom}, EdgeBottom},
                {{bounds.left, bounds.bottom}, EdgeLeft | EdgeBottom}, {{bounds.left, cy}, EdgeLeft}};
    }

    int previewHandleAt(const Rect& rect, const DisplayTransform& transform, int x, int y) const {
        const int radius = previewDpiPixels(7);
        for (const auto& handle : previewHandles(rect, transform)) {
            if (std::abs(x - handle.first.x) <= radius && std::abs(y - handle.first.y) <= radius) return handle.second;
        }
        return EdgeNone;
    }

    [[nodiscard]] CanvasTarget targetForActiveTool() const {
        return activePreviewTool_ == PreviewTool::Scrollbar
            ? CanvasTarget::Track : CanvasTarget::Viewport;
    }

    void applyPreviewDragPoint(const POINT& imagePoint) {
        if (dragTarget_ == CanvasTarget::None) return;
        const int imageWidth = std::max(1, preview_.cols);
        const int imageHeight = std::max(1, preview_.rows);
        const int dx = imagePoint.x - dragStartImage_.x;
        const int dy = imagePoint.y - dragStartImage_.y;
        Rect result = dragOriginal_;
        if (drawingReplacement_) {
            const int left = std::clamp<int>(std::min<int>(dragStartImage_.x, imagePoint.x), 0, imageWidth - 1);
            const int top = std::clamp<int>(std::min<int>(dragStartImage_.y, imagePoint.y), 0, imageHeight - 1);
            const int right = std::clamp<int>(std::max<int>(dragStartImage_.x, imagePoint.x) + 1, left + 1,
                                              imageWidth);
            const int bottom = std::clamp<int>(std::max<int>(dragStartImage_.y, imagePoint.y) + 1, top + 1,
                                               imageHeight);
            result = {left, top, right - left, bottom - top};
        } else if (dragEdges_ == EdgeNone) {
            result.x = std::clamp(dragOriginal_.x + dx, 0,
                                  std::max(0, imageWidth - dragOriginal_.width));
            result.y = std::clamp(dragOriginal_.y + dy, 0,
                                  std::max(0, imageHeight - dragOriginal_.height));
        } else {
            if (dragEdges_ & EdgeLeft) {
                const int left = std::clamp(dragOriginal_.x + dx, 0,
                                            dragOriginal_.right() - 1);
                result.x = left;
                result.width = dragOriginal_.right() - left;
            }
            if (dragEdges_ & EdgeRight) {
                result.width = std::clamp(dragOriginal_.width + dx, 1,
                                          imageWidth - dragOriginal_.x);
            }
            if (dragEdges_ & EdgeTop) {
                const int top = std::clamp(dragOriginal_.y + dy, 0,
                                           dragOriginal_.bottom() - 1);
                result.y = top;
                result.height = dragOriginal_.bottom() - top;
            }
            if (dragEdges_ & EdgeBottom) {
                result.height = std::clamp(dragOriginal_.height + dy, 1,
                                           imageHeight - dragOriginal_.y);
            }
        }
        result = constrainedPreviewRect(result);
        if (dragTarget_ == CanvasTarget::Viewport) {
            if (!sameRect(result, readRect(viewportEdits_))) gestureChanged_ = true;
            setPreviewRect(viewportEdits_, result);
            viewportCustomized_ = true;
        } else if (dragTarget_ == CanvasTarget::Track) {
            if (!sameRect(result, readRect(trackEdits_))) gestureChanged_ = true;
            setPreviewRect(trackEdits_, result);
            if (!selectedScrollbarCandidate_ && result.valid()) {
                scrollbarSide_ = result.x + result.width / 2 < preview_.cols / 2
                    ? ScrollbarSide::Left : ScrollbarSide::Right;
            }
        }
        lastImagePoint_ = imagePoint;
    }

    void beginPreviewPan(HWND canvas, int clientX, int clientY) {
        beginPreviewGesture();
        panningPreview_ = true;
        panStartClient_ = {clientX, clientY};
        panOriginalX_ = previewPanX_;
        panOriginalY_ = previewPanY_;
        SetCapture(canvas);
        updatePreviewCursor(canvas, nullptr);
    }

    void updatePreviewPan(HWND canvas, int clientX, int clientY) {
        if (!panningPreview_) return;
        const double x = panOriginalX_ + clientX - panStartClient_.x;
        const double y = panOriginalY_ + clientY - panStartClient_.y;
        if (x != previewPanX_ || y != previewPanY_) gestureChanged_ = true;
        previewPanX_ = x;
        previewPanY_ = y;
        InvalidateRect(canvas, nullptr, FALSE);
    }

    void beginPreviewDrag(HWND canvas, int clientX, int clientY) {
        if (preview_.empty()) return;
        const bool spaceGesture = spaceDown_ || (GetKeyState(VK_SPACE) & 0x8000) != 0;
        if (spaceGesture) {
            beginPreviewPan(canvas, clientX, clientY);
            return;
        }
        if (!previewCalibrationMutable()) return;

        const CanvasTarget target = targetForActiveTool();
        const Rect current = target == CanvasTarget::Track
            ? constrainedPreviewRect(readRect(trackEdits_))
            : constrainedPreviewRect(readRect(viewportEdits_));
        const auto transform = displayTransform(canvas);
        if (!transform) return;
        POINT imagePoint{};
        const bool onImage = clientToImage(canvas, clientX, clientY, imagePoint);
        const int handleEdges = previewHandleAt(current, *transform, clientX, clientY);
        if (!onImage && (drawingReplacement_ || !handleEdges)) return;
        // Handles may sit just outside a thin track or at an image edge.
        // Keep the unbounded start coordinate; result rectangles are clamped.
        imagePoint = {static_cast<LONG>(std::lround((clientX - transform->originX) / transform->scale)),
                      static_cast<LONG>(std::lround((clientY - transform->originY) / transform->scale))};
        const int radius = transform
            ? std::max(1, static_cast<int>(std::ceil(previewDpiPixels(7) / transform->scale)))
            : previewDpiPixels(7);
        int edges = EdgeNone;
        bool hit = hitRect(current, imagePoint, radius, edges);
        if (handleEdges) { hit = true; edges = handleEdges; }
        if (target == CanvasTarget::Track && transform && !(edges & (EdgeTop | EdgeBottom)) &&
            !handleEdges &&
            trackMoveGripHit(current, imagePoint, transform->scale)) {
            hit = true;
            edges = EdgeNone;
        }
        if (!hit && !drawingReplacement_) return;

        beginPreviewGesture();
        dragTarget_ = target;
        dragEdges_ = drawingReplacement_ ? EdgeNone : edges;
        dragStartImage_ = imagePoint;
        lastImagePoint_ = imagePoint;
        dragOriginal_ = current;
        SetCapture(canvas);
        updatePreviewCursor(canvas, &imagePoint);
    }

    void updatePreviewDrag(HWND canvas, int clientX, int clientY) {
        if (panningPreview_) {
            updatePreviewPan(canvas, clientX, clientY);
            return;
        }
        if (dragTarget_ == CanvasTarget::None) return;
        const auto transform = displayTransform(canvas);
        if (!transform) return;
        POINT imagePoint{static_cast<LONG>(std::lround((clientX - transform->originX) / transform->scale)),
                         static_cast<LONG>(std::lround((clientY - transform->originY) / transform->scale))};
        applyPreviewDragPoint(imagePoint);
        InvalidateRect(canvas, nullptr, FALSE);
    }

    void endPreviewDrag(HWND canvas) {
        finishPreviewGesture(false);
        if (dragTarget_ == CanvasTarget::Viewport) {
            logger_.info("calibration", "viewport gesture finished with{" +
                         rectDescription(readRect(viewportEdits_)) + "}");
        } else if (dragTarget_ == CanvasTarget::Track) {
            logger_.info("calibration", "scrollbar track gesture finished with{" +
                         rectDescription(readRect(trackEdits_)) + "}");
        }
        dragTarget_ = CanvasTarget::None;
        dragEdges_ = EdgeNone;
        if (GetCapture() == canvas) ReleaseCapture();
        InvalidateRect(canvas, nullptr, FALSE);
    }

    void updatePreviewCursor(HWND canvas, const POINT* imagePoint) const {
        if (!canvas) return;
        HCURSOR cursor = LoadCursorW(nullptr, IDC_ARROW);
        if (panningPreview_) {
            cursor = LoadCursorW(nullptr, IDC_HAND);
        } else if (dragTarget_ != CanvasTarget::None && dragEdges_ != EdgeNone) {
            const bool horizontal = (dragEdges_ & (EdgeLeft | EdgeRight)) != 0;
            const bool vertical = (dragEdges_ & (EdgeTop | EdgeBottom)) != 0;
            if (horizontal && vertical) {
                const bool descending = ((dragEdges_ & EdgeLeft) != 0) ==
                                        ((dragEdges_ & EdgeTop) != 0);
                cursor = LoadCursorW(nullptr, descending ? IDC_SIZENWSE : IDC_SIZENESW);
            } else if (horizontal) {
                cursor = LoadCursorW(nullptr, IDC_SIZEWE);
            } else {
                cursor = LoadCursorW(nullptr, IDC_SIZENS);
            }
        } else if (drawingReplacement_) {
            cursor = LoadCursorW(nullptr, IDC_CROSS);
        } else if (imagePoint && !preview_.empty()) {
            const CanvasTarget target = targetForActiveTool();
            const Rect current = target == CanvasTarget::Track
                ? constrainedPreviewRect(readRect(trackEdits_))
                : constrainedPreviewRect(readRect(viewportEdits_));
            const auto transform = displayTransform(canvas);
            const int radius = transform
                ? std::max(1, static_cast<int>(std::ceil(previewDpiPixels(7) / transform->scale)))
                : previewDpiPixels(7);
            int edges = EdgeNone;
            if (hitRect(current, *imagePoint, radius, edges)) {
                if (target == CanvasTarget::Track && transform &&
                    trackMoveGripHit(current, *imagePoint, transform->scale)) {
                    edges = EdgeNone;
                }
                if (edges == EdgeNone) cursor = LoadCursorW(nullptr, IDC_SIZEALL);
                else {
                    const bool horizontal = (edges & (EdgeLeft | EdgeRight)) != 0;
                    const bool vertical = (edges & (EdgeTop | EdgeBottom)) != 0;
                    if (horizontal && vertical) {
                        const bool descending = ((edges & EdgeLeft) != 0) ==
                                                ((edges & EdgeTop) != 0);
                        cursor = LoadCursorW(nullptr, descending ? IDC_SIZENWSE : IDC_SIZENESW);
                    } else if (horizontal) cursor = LoadCursorW(nullptr, IDC_SIZEWE);
                    else cursor = LoadCursorW(nullptr, IDC_SIZENS);
                }
            }
        }
        SetCursor(cursor);
    }

    static LPCWSTR cursorForPreviewEdges(int edges) {
        const bool horizontal = (edges & (EdgeLeft | EdgeRight)) != 0;
        const bool vertical = (edges & (EdgeTop | EdgeBottom)) != 0;
        if (horizontal && vertical) return ((edges & EdgeLeft) != 0) == ((edges & EdgeTop) != 0) ? IDC_SIZENWSE : IDC_SIZENESW;
        return horizontal ? IDC_SIZEWE : IDC_SIZENS;
    }

    void refreshScrollbarCandidateCombo() {
        if (!scrollbarCandidateCombo_) return;
        SendMessageW(scrollbarCandidateCombo_, CB_RESETCONTENT, 0, 0);
        for (std::size_t index = 0; index < scrollbarCandidates_.size(); ++index) {
            const auto& candidate = scrollbarCandidates_[index];
            std::wostringstream entry;
            entry << L"Rank " << index + 1 << L" of " << scrollbarCandidates_.size() << L' '
                  << (candidate.config.side == ScrollbarSide::Left ? L"Left" : L"Right")
                  << L" | x=" << candidate.config.track.x
                  << L" y=" << candidate.config.track.y
                  << L" " << candidate.config.track.width << L'x' << candidate.config.track.height
                  << L" | match score "
                  << std::fixed << std::setprecision(3) << candidate.autoDetectionScore;
            SendMessageW(scrollbarCandidateCombo_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(entry.str().c_str()));
        }
        if (selectedScrollbarCandidate_ &&
            *selectedScrollbarCandidate_ < scrollbarCandidates_.size()) {
            SendMessageW(scrollbarCandidateCombo_, CB_SETCURSEL,
                         static_cast<WPARAM>(*selectedScrollbarCandidate_), 0);
        } else {
            SendMessageW(scrollbarCandidateCombo_, CB_SETCURSEL,
                         static_cast<WPARAM>(-1), 0);
        }
        if (HWND setButton = GetDlgItem(window_, kSetScrollbarCandidate)) {
            EnableWindow(setButton, !scrollbarCandidates_.empty());
        }
        if (previewPreviousButton_) {
            EnableWindow(previewPreviousButton_, scrollbarCandidates_.size() > 1);
            EnableWindow(previewNextButton_, scrollbarCandidates_.size() > 1);
        }
    }

    void selectScrollbarCandidate(std::size_t index, bool announce) {
        if (index >= scrollbarCandidates_.size()) return;
        if (!previewCalibrationMutable()) return;
        const bool changing = !selectedScrollbarCandidate_ || *selectedScrollbarCandidate_ != index ||
            !sameRect(readRect(trackEdits_), constrainedPreviewRect(scrollbarCandidates_[index].config.track));
        if (changing && !historyRestoreInProgress_) {
            rememberCalibration();
        }
        selectedScrollbarCandidate_ = index;
        if (scrollbarCandidateCombo_) {
            SendMessageW(scrollbarCandidateCombo_, CB_SETCURSEL,
                         static_cast<WPARAM>(index), 0);
        }
        const auto& candidate = scrollbarCandidates_[index];
        scrollbarSide_ = candidate.config.side;
        const Rect track = constrainedPreviewRect(candidate.config.track);
        setPreviewRect(trackEdits_, track);
        // A candidate switch should not throw away a manual crop. The derived
        // viewport is refreshed only while the crop is still untouched.
        if (!viewportCustomized_) setPreviewRect(viewportEdits_, viewportForTrack(track));
        logger_.info("calibration", "selected scrollbar candidate #" + std::to_string(index + 1) +
                     " track{" + rectDescription(track) + "} thumb{" +
                     rectDescription(candidate.observation.thumb) + "} confidence=" +
                     std::to_string(candidate.observation.confidence));
        refreshScrollbarCandidateCombo();
        refreshWorkflow();
        InvalidateRect(previewCanvas_, nullptr, FALSE);
        if (announce) {
            std::wostringstream message;
            message << L"Selected scrollbar: rank " << index + 1 << L" of "
                    << scrollbarCandidates_.size() << L", match score "
                    << std::fixed << std::setprecision(3) << candidate.autoDetectionScore
                    << L". Choose Scrollbar in the preview to edit it.";
            setStatus(message.str());
        }
    }

    void selectAdjacentScrollbarCandidate(int direction) {
        if (scrollbarCandidates_.empty() || direction == 0) return;
        const int count = static_cast<int>(scrollbarCandidates_.size());
        const int current = selectedScrollbarCandidate_
            ? static_cast<int>(*selectedScrollbarCandidate_) : (direction > 0 ? -1 : 0);
        int next = (current + (direction > 0 ? 1 : -1)) % count;
        if (next < 0) next += count;
        selectScrollbarCandidate(static_cast<std::size_t>(next), true);
    }

    void setScrollbarCandidateFromCombo() {
        if (!scrollbarCandidateCombo_) return;
        const LRESULT selected = SendMessageW(scrollbarCandidateCombo_, CB_GETCURSEL, 0, 0);
        if (selected == CB_ERR || selected < 0 ||
            static_cast<std::size_t>(selected) >= scrollbarCandidates_.size()) {
            setStatus(L"Run auto-detection and choose a scrollbar candidate first.");
            return;
        }
        selectScrollbarCandidate(static_cast<std::size_t>(selected), true);
    }

    void updatePreviewToolButtons() const {
        if (!previewToolbarCreated_) return;
        const bool editable = previewCalibrationMutable() && !preview_.empty();
        const bool trackTool = activePreviewTool_ == PreviewTool::Scrollbar;
        for (HWND button : {previewToolCaptureButton_, previewToolScrollbarButton_, previewDrawButton_})
            EnableWindow(button, editable);
        EnableWindow(previewUndoButton_, editable && (!calibrationHistory_.empty() || gestureActive_));
        EnableWindow(previewResetButton_, editable && !trackTool);
        for (HWND button : {previewPreviousButton_, previewNextButton_}) {
            ShowWindow(button, trackTool ? SW_SHOWNA : SW_HIDE);
            EnableWindow(button, editable && !scrollbarCandidates_.empty());
        }
        std::wstring hint;
        if (!editable) hint = L"Preview only. Wheel to zoom; hold Space and drag to pan.";
        else if (drawingReplacement_) hint = trackTool
            ? L"Drag around the full scrollbar track, not just the moving thumb. Escape cancels."
            : L"Drag a new capture area. Escape cancels.";
        else if (trackTool) {
            hint = selectedScrollbarCandidate_
                ? L"Rank " + std::to_wstring(*selectedScrollbarCandidate_ + 1) + L" of " + std::to_wstring(scrollbarCandidates_.size()) + L" (best first). "
                : L"Manual scrollbar. ";
            hint += L"Select full track. Wheel: zoom / Space + drag: pan.";
        } else hint = L"Drag to move; use handles to resize. Wheel: zoom / Space + drag: pan.";
        setUiText(previewToolbarLabel_, hint.c_str());
        for (HWND button : {previewToolCaptureButton_, previewToolScrollbarButton_, previewDrawButton_, previewUndoButton_})
            InvalidateRect(button, nullptr, FALSE);
    }

    [[nodiscard]] int previewDpiPixels(int logical) const {
        return MulDiv(logical, static_cast<int>(previewDpi_), USER_DEFAULT_SCREEN_DPI);
    }

    void updatePreviewFont(HWND canvas) {
        UINT dpi = GetDpiForWindow(canvas);
        if (!dpi) dpi = USER_DEFAULT_SCREEN_DPI;
        previewDpi_ = dpi;
        HFONT replacement = CreateFontW(-previewDpiPixels(15), 0, 0, 0, FW_NORMAL,
                                        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                        L"Segoe UI");
        if (replacement) {
            if (previewBodyFont_) DeleteObject(previewBodyFont_);
            previewBodyFont_ = replacement;
        }
        for (HWND control : {previewToolCaptureButton_, previewToolScrollbarButton_,
                             previewDrawButton_, previewUndoButton_, previewResetButton_,
                             previewFitButton_, previewActualButton_, previewPreviousButton_,
                             previewNextButton_, previewToolbarLabel_}) {
            if (control && previewBodyFont_) {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(previewBodyFont_), TRUE);
            }
        }
    }

    HWND addPreviewButton(HWND parent, const wchar_t* text, int id) {
        HWND button = CreateWindowExW(0, L"BUTTON", text,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 80, 30, parent, menuId(id), instance_, nullptr);
        if (previewBodyFont_) SendMessageW(button, WM_SETFONT,
                                           reinterpret_cast<WPARAM>(previewBodyFont_), TRUE);
        return button;
    }

    void createPreviewToolbar(HWND canvas) {
        if (previewToolbarCreated_) return;
        updatePreviewFont(canvas);
        previewToolCaptureButton_ = addPreviewButton(canvas, L"Capture area", kPreviewToolbarCapture);
        previewToolScrollbarButton_ = addPreviewButton(canvas, L"Scrollbar", kPreviewToolbarScrollbar);
        previewDrawButton_ = addPreviewButton(canvas, L"Draw new", kPreviewToolbarDraw);
        previewUndoButton_ = addPreviewButton(canvas, L"Undo", kPreviewToolbarUndo);
        previewResetButton_ = addPreviewButton(canvas, L"Reset area", kPreviewToolbarReset);
        previewFitButton_ = addPreviewButton(canvas, L"Fit", kPreviewToolbarFit);
        previewActualButton_ = addPreviewButton(canvas, L"100%", kPreviewToolbarActual);
        previewPreviousButton_ = addPreviewButton(canvas, L"Previous", kPreviewToolbarPrevious);
        previewNextButton_ = addPreviewButton(canvas, L"Next", kPreviewToolbarNext);
        previewToolbarLabel_ = CreateWindowExW(0, L"STATIC", L"Wheel: zoom   Space + drag: pan",
                                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                0, 0, 260, 24, canvas, nullptr, instance_, nullptr);
        if (previewBodyFont_) SendMessageW(previewToolbarLabel_, WM_SETFONT,
                                           reinterpret_cast<WPARAM>(previewBodyFont_), TRUE);
        previewToolbarCreated_ = true;
        previewToolbarHeight_ = previewDpiPixels(kPreviewToolbarHeight);
        updatePreviewToolButtons();
        layoutPreviewToolbar(canvas);
    }

    void layoutPreviewToolbar(HWND canvas) {
        if (!previewToolbarCreated_ || !canvas) return;
        RECT client{};
        GetClientRect(canvas, &client);
        const int width = client.right;
        previewToolbarHeight_ = previewDpiPixels(104);
        int x = previewDpiPixels(12);
        auto place = [&](HWND control, int logicalWidth) {
            const int w = previewDpiPixels(logicalWidth);
            MoveWindow(control, x, previewDpiPixels(10), w, previewDpiPixels(34), TRUE);
            x += w + previewDpiPixels(6);
        };
        place(previewToolCaptureButton_, 112);
        place(previewToolScrollbarButton_, 94);
        place(previewDrawButton_, 88);
        place(previewUndoButton_, 62);
        place(previewResetButton_, 90);
        place(previewFitButton_, 54);
        place(previewActualButton_, 60);
        MoveWindow(previewToolbarLabel_, previewDpiPixels(14), previewDpiPixels(56),
                   std::max(previewDpiPixels(250), width - previewDpiPixels(210)),
                   previewDpiPixels(40), TRUE);
        MoveWindow(previewPreviousButton_, width - previewDpiPixels(190), previewDpiPixels(57),
                   previewDpiPixels(96), previewDpiPixels(32), TRUE);
        MoveWindow(previewNextButton_, width - previewDpiPixels(86), previewDpiPixels(57),
                   previewDpiPixels(72), previewDpiPixels(32), TRUE);
        updatePreviewToolButtons();
    }

    void paintPreviewButton(const DRAWITEMSTRUCT* item) const {
        if (!item) return;
        FillRect(item->hDC, &item->rcItem, backgroundBrush_);
        RECT rect = item->rcItem;
        InflateRect(&rect, -1, -1);
        const bool selected = (item->CtlID == kPreviewToolbarCapture && activePreviewTool_ == PreviewTool::CaptureArea) ||
            (item->CtlID == kPreviewToolbarScrollbar && activePreviewTool_ == PreviewTool::Scrollbar) ||
            (item->CtlID == kPreviewToolbarDraw && drawingReplacement_);
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        COLORREF fill = selected ? RGB(117, 171, 255) : RGB(34, 41, 53);
        COLORREF ink = selected ? RGB(14, 29, 51) : RGB(225, 233, 246);
        if (disabled) { fill = RGB(30, 35, 43); ink = RGB(109, 120, 137); }
        else if (item->itemState & ODS_SELECTED) fill = RGB(70, 111, 171);
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, selected ? fill : RGB(54, 64, 80));
        HGDIOBJ oldBrush = SelectObject(item->hDC, brush), oldPen = SelectObject(item->hDC, pen);
        RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom, previewDpiPixels(9), previewDpiPixels(9));
        SelectObject(item->hDC, oldPen); SelectObject(item->hDC, oldBrush);
        DeleteObject(pen); DeleteObject(brush);
        HGDIOBJ oldFont = SelectObject(item->hDC, previewBodyFont_);
        SetBkMode(item->hDC, TRANSPARENT);
        SetTextColor(item->hDC, ink);
        wchar_t caption[96]{};
        GetWindowTextW(item->hwndItem, caption, static_cast<int>(std::size(caption)));
        DrawTextW(item->hDC, caption, -1, &rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item->hDC, oldFont);
        if (item->itemState & ODS_FOCUS) {
            InflateRect(&rect, -4, -4);
            DrawFocusRect(item->hDC, &rect);
        }
    }

    void createPreviewControls(HWND owner) {
        if (previewCanvas_) {
            ShowWindow(previewCanvas_, SW_SHOW);
            UpdateWindow(previewCanvas_);
            return;
        }
        if (!instance_) return;
        const wchar_t className[] = L"UniversalScrollStitcherPreview";
        WNDCLASSEXW existing{sizeof(existing)};
        if (!GetClassInfoExW(instance_, className, &existing)) {
            WNDCLASSEXW klass{sizeof(klass)};
            klass.lpfnWndProc = &MainWindow::previewProc;
            klass.hInstance = instance_;
            klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            klass.lpszClassName = className;
            RegisterClassExW(&klass);
        }
        previewCanvas_ = CreateWindowExW(
            WS_EX_TOOLWINDOW, className,
            L"Preview & calibration",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            900, 680, owner ? owner : window_, nullptr, instance_, this);
        if (previewCanvas_) {
            ShowWindow(previewCanvas_, SW_SHOW);
            UpdateWindow(previewCanvas_);
        }
    }

    static void drawDimmedRect(HDC dc, const RECT& rect) {
        if (!dc || rect.right <= rect.left || rect.bottom <= rect.top) return;
        HDC source = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, rect.right - rect.left,
                                                rect.bottom - rect.top);
        if (!source || !bitmap) {
            if (bitmap) DeleteObject(bitmap);
            if (source) DeleteDC(source);
            return;
        }
        HGDIOBJ old = SelectObject(source, bitmap);
        RECT sourceRect{0, 0, rect.right - rect.left, rect.bottom - rect.top};
        HBRUSH brush = CreateSolidBrush(RGB(14, 17, 21));
        FillRect(source, &sourceRect, brush);
        DeleteObject(brush);
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 145, 0};
        AlphaBlend(dc, rect.left, rect.top, sourceRect.right, sourceRect.bottom,
                   source, 0, 0, sourceRect.right, sourceRect.bottom, blend);
        SelectObject(source, old);
        DeleteObject(bitmap);
        DeleteDC(source);
    }

    static RECT intersectRect(const RECT& left, const RECT& right) {
        RECT result{std::max<LONG>(left.left, right.left), std::max<LONG>(left.top, right.top),
                    std::min<LONG>(left.right, right.right), std::min<LONG>(left.bottom, right.bottom)};
        if (result.right < result.left) result.right = result.left;
        if (result.bottom < result.top) result.bottom = result.top;
        return result;
    }

    void dimOutsideViewport(HDC dc, const RECT& imageRect, const RECT& viewportRect,
                            const RECT& content) const {
        const RECT visibleImage = intersectRect(imageRect, content);
        const RECT visibleViewport = intersectRect(viewportRect, visibleImage);
        if (visibleImage.right <= visibleImage.left || visibleImage.bottom <= visibleImage.top) return;
        drawDimmedRect(dc, {visibleImage.left, visibleImage.top,
                            visibleImage.right, std::max(visibleImage.top, visibleViewport.top)});
        drawDimmedRect(dc, {visibleImage.left, std::min(visibleImage.bottom, visibleViewport.bottom),
                            visibleImage.right, visibleImage.bottom});
        if (visibleViewport.top < visibleViewport.bottom) {
            drawDimmedRect(dc, {visibleImage.left, visibleViewport.top,
                                std::max(visibleImage.left, visibleViewport.left),
                                visibleViewport.bottom});
            drawDimmedRect(dc, {std::min(visibleImage.right, visibleViewport.right),
                                visibleViewport.top, visibleImage.right, visibleViewport.bottom});
        }
    }

    void drawHandle(HDC dc, int x, int y, COLORREF color) const {
        const int half = previewDpiPixels(kPreviewHandlePixels) / 2;
        RECT handle{x - half, y - half, x + half + 1, y + half + 1};
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &handle, brush);
        DeleteObject(brush);
        FrameRect(dc, &handle, reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    void drawPreviewTag(HDC dc, const wchar_t* text, int x, int y, int maxWidth, COLORREF ink) const {
        if (maxWidth < previewDpiPixels(50)) return;
        HGDIOBJ oldFont = SelectObject(dc, previewBodyFont_ ? previewBodyFont_ : GetStockObject(DEFAULT_GUI_FONT));
        SIZE extent{};
        GetTextExtentPoint32W(dc, text, static_cast<int>(wcslen(text)), &extent);
        RECT box{x, y, x + std::min<int>(maxWidth, extent.cx + previewDpiPixels(16)), y + previewDpiPixels(26)};
        FillRect(dc, &box, backgroundBrush_);
        box.left += previewDpiPixels(8); box.right -= previewDpiPixels(4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ink);
        DrawTextW(dc, text, -1, &box, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
    }

    void paintPreview(HWND canvas, HDC dc) const {
        RECT client{};
        GetClientRect(canvas, &client);
        if (client.right <= 0 || client.bottom <= 0) return;

        // Compose the image, dimming and handles before touching the window.
        // Painting the clear directly to the window exposes a blank frame on
        // every live refresh or drag, even when WM_ERASEBKGND is suppressed.
        HDC buffer = CreateCompatibleDC(dc);
        HBITMAP bitmap = buffer ? CreateCompatibleBitmap(dc, client.right, client.bottom) : nullptr;
        HGDIOBJ previous = bitmap ? SelectObject(buffer, bitmap) : nullptr;
        if (previous && previous != HGDI_ERROR) {
            paintPreviewContents(canvas, buffer);
            BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
            SelectObject(buffer, previous);
        } else {
            // Preserve a usable preview if GDI cannot allocate the buffer.
            paintPreviewContents(canvas, dc);
        }
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
    }

    void paintPreviewContents(HWND canvas, HDC dc) const {
        RECT client{};
        GetClientRect(canvas, &client);
        HBRUSH background = CreateSolidBrush(RGB(20, 23, 29));
        FillRect(dc, &client, background);
        DeleteObject(background);
        if (preview_.empty()) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(205, 213, 224));
            DrawTextW(dc, L"Capture a preview to display the target and calibration rectangles.", -1,
                      &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return;
        }
        const auto transform = displayTransform(canvas);
        if (!transform) return;
        const RECT& content = transform->content;
        const int save = SaveDC(dc);
        IntersectClipRect(dc, content.left, content.top, content.right, content.bottom);
        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = preview_.cols;
        bitmapInfo.bmiHeader.biHeight = -preview_.rows;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        const RECT imageRect = transform->destination;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, imageRect.left, imageRect.top,
                      imageRect.right - imageRect.left, imageRect.bottom - imageRect.top,
                      0, 0, preview_.cols, preview_.rows, preview_.data, &bitmapInfo,
                      DIB_RGB_COLORS, SRCCOPY);

        const Rect viewport = constrainedPreviewRect(readRect(viewportEdits_));
        const Rect track = constrainedPreviewRect(readRect(trackEdits_));
        const RECT viewportRect = scaledRect(viewport, *transform);
        const RECT trackRect = scaledRect(track, *transform);
        dimOutsideViewport(dc, imageRect, viewportRect, content);

        HPEN viewportPen = CreatePen(PS_SOLID, 2,
                                     activePreviewTool_ == PreviewTool::CaptureArea
                                         ? RGB(66, 226, 130) : RGB(50, 153, 92));
        HPEN trackPen = CreatePen(PS_SOLID, 2,
                                 activePreviewTool_ == PreviewTool::Scrollbar
                                     ? RGB(255, 183, 67) : RGB(178, 119, 42));
        HGDIOBJ oldPen = SelectObject(dc, viewportPen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, viewportRect.left, viewportRect.top, viewportRect.right, viewportRect.bottom);
        SelectObject(dc, trackPen);
        Rectangle(dc, trackRect.left, trackRect.top, trackRect.right, trackRect.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(viewportPen);
        DeleteObject(trackPen);

        if (previewCalibrationMutable()) {
            const bool isContent = activePreviewTool_ == PreviewTool::CaptureArea;
            const COLORREF color = isContent ? RGB(66, 226, 130) : RGB(255, 183, 67);
            for (const auto& handle : previewHandles(isContent ? viewport : track, *transform))
                drawHandle(dc, handle.first.x, handle.first.y, color);
            if (!isContent && trackRect.right - trackRect.left < previewDpiPixels(30)) {
                const int cx = (trackRect.left + trackRect.right) / 2, cy = (trackRect.top + trackRect.bottom) / 2;
                drawHandle(dc, cx, cy, RGB(255, 183, 67));
            }
        }

        // Describe exactly the core's header + moving-band + excluded-footer
        // behavior, rather than implying every selected row repeats.
        HPEN boundaryPen = CreatePen(PS_DASH, 1, RGB(220, 226, 236));
        oldPen = SelectObject(dc, boundaryPen);
        if (viewport.y < track.y && track.y < viewport.bottom()) {
            MoveToEx(dc, viewportRect.left, trackRect.top, nullptr);
            LineTo(dc, viewportRect.right, trackRect.top);
        }
        if (track.bottom() < viewport.bottom()) {
            drawDimmedRect(dc, intersectRect({viewportRect.left, trackRect.bottom, viewportRect.right, viewportRect.bottom}, content));
            MoveToEx(dc, viewportRect.left, trackRect.bottom, nullptr);
            LineTo(dc, viewportRect.right, trackRect.bottom);
        }
        SelectObject(dc, oldPen);
        DeleteObject(boundaryPen);
        const int labelX = std::max<LONG>(content.left + previewDpiPixels(8), viewportRect.left + previewDpiPixels(8));
        const int labelWidth = std::min<LONG>(viewportRect.right, content.right) - labelX - previewDpiPixels(8);
        if (viewport.y < track.y) drawPreviewTag(dc, L"Header - included once", labelX,
            std::max<LONG>(content.top + previewDpiPixels(8), viewportRect.top + previewDpiPixels(8)), labelWidth, RGB(149, 204, 255));
        drawPreviewTag(dc, L"Scrolling content", labelX,
            std::max<LONG>(content.top + previewDpiPixels(8), std::max(viewportRect.top, trackRect.top) + previewDpiPixels(8)),
            labelWidth, RGB(66, 226, 130));
        if (track.bottom() < viewport.bottom()) drawPreviewTag(dc, L"Excluded", labelX,
            trackRect.bottom + previewDpiPixels(8), labelWidth, RGB(159, 171, 188));
        const int trackLabelX = std::clamp<int>(trackRect.left - previewDpiPixels(165), content.left + previewDpiPixels(8),
                                                std::max<int>(content.left + previewDpiPixels(8), content.right - previewDpiPixels(175)));
        drawPreviewTag(dc, L"Scrollbar track", trackLabelX,
            std::max<LONG>(content.top + previewDpiPixels(8), trackRect.top + previewDpiPixels(8)),
            previewDpiPixels(165), RGB(255, 183, 67));
        RestoreDC(dc, save);
    }

    LRESULT handlePreviewMessage(HWND canvas, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            createPreviewToolbar(canvas);
            return 0;
        case WM_CLOSE:
            cancelPreviewGesture(canvas);
            ShowWindow(canvas, SW_HIDE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize.x = previewDpiPixels(kPreviewMinWidth);
            limits->ptMinTrackSize.y = previewDpiPixels(kPreviewMinHeight);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(canvas, &paint);
            paintPreview(canvas, dc);
            EndPaint(canvas, &paint);
            return 0;
        }
        case WM_DRAWITEM:
            paintPreviewButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            switch (LOWORD(wParam)) {
            case kPreviewToolbarCapture: setActivePreviewTool(PreviewTool::CaptureArea); break;
            case kPreviewToolbarScrollbar: setActivePreviewTool(PreviewTool::Scrollbar); break;
            case kPreviewToolbarDraw: startDrawReplacement(); break;
            case kPreviewToolbarUndo: undoPreviewCalibration(); break;
            case kPreviewToolbarReset: resetActivePreviewArea(); break;
            case kPreviewToolbarFit: fitPreview(); break;
            case kPreviewToolbarActual: actualSizePreview(); break;
            case kPreviewToolbarPrevious: selectAdjacentScrollbarCandidate(-1); break;
            case kPreviewToolbarNext: selectAdjacentScrollbarCandidate(1); break;
            default: return DefWindowProcW(canvas, message, wParam, lParam);
            }
            return 0;
        }
        case WM_SIZE:
            cancelPreviewGesture(canvas);
            layoutPreviewToolbar(canvas);
            InvalidateRect(canvas, nullptr, TRUE);
            return 0;
        case WM_DPICHANGED: {
            cancelPreviewGesture(canvas);
            updatePreviewFont(canvas);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) SetWindowPos(canvas, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            layoutPreviewToolbar(canvas);
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lParam) != canvas && previewInteractionActive()) cancelPreviewGesture(canvas);
            return 0;
        case WM_CANCELMODE:
            cancelPreviewGesture(canvas);
            spaceDown_ = false;
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                cancelPreviewGesture(canvas);
                spaceDown_ = false;
            }
            return 0;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkColor(dc, RGB(20, 23, 29));
            SetTextColor(dc, RGB(159, 171, 188));
            return reinterpret_cast<LRESULT>(backgroundBrush_);
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                cancelPreviewGesture(canvas);
                return 0;
            }
            if (wParam == VK_SPACE) {
                spaceDown_ = true;
                updatePreviewCursor(canvas, nullptr);
                return 0;
            }
            if ((wParam == 'Z' || wParam == 'z') && (GetKeyState(VK_CONTROL) & 0x8000)) {
                undoPreviewCalibration();
                return 0;
            }
            return 0;
        case WM_KEYUP:
            if (wParam == VK_SPACE) {
                spaceDown_ = false;
                updatePreviewCursor(canvas, nullptr);
                return 0;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            POINT cursor{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(canvas, &cursor);
            zoomPreviewAt(canvas, cursor.x, cursor.y,
                          GET_WHEEL_DELTA_WPARAM(wParam));
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(canvas);
            beginPreviewDrag(canvas, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE: {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (GetCapture() == canvas) updatePreviewDrag(canvas, x, y);
            else {
                const auto transform = displayTransform(canvas);
                if (transform && previewCalibrationMutable() && !drawingReplacement_ && !spaceDown_) {
                    const Rect active = readRect(activePreviewTool_ == PreviewTool::CaptureArea ? viewportEdits_ : trackEdits_);
                    const int edges = previewHandleAt(active, *transform, x, y);
                    if (edges) {
                        SetCursor(LoadCursorW(nullptr, cursorForPreviewEdges(edges)));
                        return 0;
                    }
                }
                POINT image{};
                if (clientToImage(canvas, x, y, image)) updatePreviewCursor(canvas, &image);
                else updatePreviewCursor(canvas, nullptr);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            endPreviewDrag(canvas);
            return 0;
        case WM_RBUTTONDOWN:
            cancelPreviewGesture(canvas);
            return 0;
        case WM_NCDESTROY:
            cancelPreviewGesture(canvas);
            previewToolbarCreated_ = false;
            previewToolCaptureButton_ = nullptr;
            previewToolScrollbarButton_ = nullptr;
            previewDrawButton_ = nullptr;
            previewUndoButton_ = nullptr;
            previewResetButton_ = nullptr;
            previewFitButton_ = nullptr;
            previewActualButton_ = nullptr;
            previewPreviousButton_ = nullptr;
            previewNextButton_ = nullptr;
            previewToolbarLabel_ = nullptr;
            previewToolbarHeight_ = 0;
            if (previewBodyFont_) {
                DeleteObject(previewBodyFont_);
                previewBodyFont_ = nullptr;
            }
            if (previewCanvas_ == canvas) previewCanvas_ = nullptr;
            return DefWindowProcW(canvas, message, wParam, lParam);
        default:
            return DefWindowProcW(canvas, message, wParam, lParam);
        }
    }
