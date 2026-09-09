// Exercise preview geometry and gesture handlers with hidden Win32 controls.
#include "../src/main.cpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace universal_stitcher {
namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool nearlyEqual(double left, double right, double tolerance = 1.0) {
    return std::abs(left - right) <= tolerance;
}

struct PreviewInteractionTests {
    MainWindow app;
    HWND canvas = nullptr;

    void setRectangles(const Rect& viewport, const Rect& track) {
        app.setRect(app.viewportEdits_, viewport);
        app.setRect(app.trackEdits_, track);
    }

    POINT imagePoint(double x, double y) const {
        return app.imageToClient(canvas, x, y);
    }

    void setup() {
        canvas = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                                0, 0, 416, 416, nullptr, nullptr, nullptr, nullptr);
        require(canvas != nullptr, "Cannot create test canvas");
        app.previewCanvas_ = canvas;
        for (auto* edits : {&app.trackEdits_, &app.viewportEdits_}) {
            for (auto& edit : *edits) {
                edit = CreateWindowExW(0, L"EDIT", L"0", WS_CHILD,
                                       0, 0, 40, 20, canvas, nullptr, nullptr, nullptr);
                require(edit != nullptr, "Cannot create test edit");
            }
        }
        app.preview_ = cv::Mat(400, 400, CV_8UC4);
        ScrollbarCandidate selected{}, nearby{};
        selected.config.side = ScrollbarSide::Right;
        selected.config.track = {200, 50, 24, 280};
        nearby.config.side = ScrollbarSide::Right;
        nearby.config.track = {228, 50, 24, 280};
        app.scrollbarCandidates_ = {selected, nearby};
        app.selectedScrollbarCandidate_ = 0;
        app.setActivePreviewTool(MainWindow::PreviewTool::Scrollbar);
        setRectangles({0, 0, 190, 330}, selected.config.track);
    }

    void teardown() {
        if (canvas) DestroyWindow(canvas);
        canvas = nullptr;
    }

    void resetGestureState() {
        app.cancelPreviewGesture(canvas);
        app.calibrationHistory_.clear();
        app.selectedScrollbarCandidate_ = 0;
        app.spaceDown_ = false;
        app.previewZoom_ = 0.0;
        app.previewPanX_ = 0.0;
        app.previewPanY_ = 0.0;
        app.previewZoomFit_ = true;
        app.drawingReplacement_ = false;
        app.viewportCustomized_ = false;
        app.setActivePreviewTool(MainWindow::PreviewTool::Scrollbar);
    }

    void testZoomAnchorAndPan() {
        app.fitPreview();
        const POINT cursor = imagePoint(123, 177);
        const auto before = app.displayTransform(canvas);
        require(before.has_value(), "Fit transform missing");
        const double imageX = (cursor.x - before->originX) / before->scale;
        const double imageY = (cursor.y - before->originY) / before->scale;
        app.zoomPreviewAt(canvas, cursor.x, cursor.y, WHEEL_DELTA);
        const auto after = app.displayTransform(canvas);
        require(after.has_value() && after->scale > before->scale,
                "Wheel did not zoom the original capture");
        require(nearlyEqual(after->originX + imageX * after->scale, cursor.x, 0.01) &&
                    nearlyEqual(after->originY + imageY * after->scale, cursor.y, 0.01),
                "Wheel zoom did not keep the cursor anchored");

        app.fitPreview();
        const double oldPanX = app.previewPanX_;
        app.spaceDown_ = true;
        app.beginPreviewDrag(canvas, cursor.x, cursor.y);
        require(app.panningPreview_ && app.previewInteractionActive(),
                "Space drag did not lock a pan gesture");
        app.updatePreviewDrag(canvas, cursor.x + 23, cursor.y + 11);
        app.endPreviewDrag(canvas);
        app.spaceDown_ = false;
        require(nearlyEqual(app.previewPanX_, oldPanX + 23, 0.01) &&
                    nearlyEqual(app.previewPanY_, 11, 0.01),
                "Pan did not follow the locked mouse gesture");
    }

    void testActiveToolAndNoCandidateSteal() {
        // The old regression clicked a nearby detected candidate while the
        // selected track was being resized. The active tool owns the gesture.
        app.fitPreview();
        app.setActivePreviewTool(MainWindow::PreviewTool::Scrollbar);
        setRectangles({0, 0, 190, 330}, {200, 50, 24, 280});
        const POINT edge = imagePoint(224, 150);
        app.beginPreviewDrag(canvas, edge.x, edge.y);
        require(app.selectedScrollbarCandidate_ == 0 &&
                    app.dragTarget_ == MainWindow::CanvasTarget::Track,
                "Track resize was stolen by a nearby candidate");
        app.updatePreviewDrag(canvas, imagePoint(244, 150).x, imagePoint(244, 150).y);
        app.endPreviewDrag(canvas);
        require(app.readRect(app.trackEdits_).width == 44 &&
                    app.selectedScrollbarCandidate_ == 0,
                "Selected track resize did not commit");

        // A viewport click cannot edit the scrollbar while Capture area is
        // selected, and a candidate outside the active rectangle is never
        // implicitly picked.
        app.setActivePreviewTool(MainWindow::PreviewTool::CaptureArea);
        const Rect before = app.readRect(app.viewportEdits_);
        app.beginPreviewDrag(canvas, imagePoint(220, 160).x, imagePoint(220, 160).y);
        require(app.dragTarget_ == MainWindow::CanvasTarget::None &&
                    app.selectedScrollbarCandidate_ == 0 &&
                    app.readRect(app.viewportEdits_).x == before.x,
                "Inactive tool or invisible candidate received a drag");
    }

    void testDrawCancelUndoAndBounds() {
        app.setActivePreviewTool(MainWindow::PreviewTool::CaptureArea);
        setRectangles({20, 30, 150, 180}, {360, 10, 1, 250});
        const Rect original = app.readRect(app.viewportEdits_);
        app.startDrawReplacement();
        require(!app.previewInteractionActive(), "Waiting for Draw new froze live preview");
        const POINT start = imagePoint(80, 90);
        app.beginPreviewDrag(canvas, start.x, start.y);
        app.updatePreviewDrag(canvas, imagePoint(260, 270).x, imagePoint(260, 270).y);
        require(app.previewInteractionActive(), "Draw new did not start a gesture");
        app.handlePreviewMessage(canvas, WM_KEYDOWN, VK_ESCAPE, 0);
        require(!app.previewInteractionActive() &&
                    MainWindow::sameRect(app.readRect(app.viewportEdits_), original),
                "Escape did not cancel and restore the draw gesture");

        app.startDrawReplacement();
        app.beginPreviewDrag(canvas, start.x, start.y);
        app.updatePreviewDrag(canvas, imagePoint(260, 270).x, imagePoint(260, 270).y);
        app.endPreviewDrag(canvas);
        const Rect replacement = app.readRect(app.viewportEdits_);
        require(replacement.x == 80 && replacement.y == 90 && replacement.width == 181 &&
                    replacement.height == 181,
                "Draw new did not produce the replacement rectangle");
        app.undoPreviewCalibration();
        require(MainWindow::sameRect(app.readRect(app.viewportEdits_), original),
                "Undo did not restore the pre-draw rectangle");

        // Geometry is always constrained to the original capture, including
        // a zero-width or fully out-of-range request.
        const Rect clipped = app.constrainedPreviewRect({-100, -20, 900, 900});
        require(MainWindow::sameRect(clipped, {0, 0, 400, 400}),
                "Out-of-range geometry was not clipped to the image");
        const Rect onePixel = app.constrainedPreviewRect({1000, 1000, 0, 0});
        require(onePixel.x == 399 && onePixel.y == 399 && onePixel.width == 1 &&
                    onePixel.height == 1,
                "Invalid geometry did not retain a bounded one-pixel area");
    }

    void testThinTrackAndCropPreservation() {
        app.fitPreview();
        app.setActivePreviewTool(MainWindow::PreviewTool::Scrollbar);
        setRectangles({0, 0, 190, 330}, {350, 30, 1, 250});
        // The top and bottom handles remain resize handles even when the
        // track is only one source pixel wide.
        const POINT top = imagePoint(350, 30);
        app.beginPreviewDrag(canvas, top.x, top.y);
        require(app.dragTarget_ == MainWindow::CanvasTarget::Track &&
                    (app.dragEdges_ & MainWindow::EdgeTop) != 0,
                "Thin scrollbar top handle became a move gesture");
        app.updatePreviewDrag(canvas, imagePoint(350, 50).x, imagePoint(350, 50).y);
        app.endPreviewDrag(canvas);
        Rect topResized = app.readRect(app.trackEdits_);
        require(topResized.x == 350 && topResized.width == 1 && topResized.y == 50 &&
                    topResized.height == 230,
                "Thin scrollbar top handle did not resize");

        const POINT bottom = imagePoint(350, topResized.bottom());
        app.beginPreviewDrag(canvas, bottom.x, bottom.y);
        require(app.dragTarget_ == MainWindow::CanvasTarget::Track &&
                    (app.dragEdges_ & MainWindow::EdgeBottom) != 0,
                "Thin scrollbar bottom handle became a move gesture");
        app.updatePreviewDrag(canvas, imagePoint(350, 260).x, imagePoint(350, 260).y);
        app.endPreviewDrag(canvas);
        Rect bottomResized = app.readRect(app.trackEdits_);
        require(bottomResized.x == 350 && bottomResized.width == 1 &&
                    bottomResized.y == 50 && bottomResized.height == 210,
                "Thin scrollbar bottom handle did not resize");

        setRectangles({0, 0, 190, 330}, {350, 30, 1, 250});
        const POINT grip = imagePoint(350, 150);
        app.beginPreviewDrag(canvas, grip.x, grip.y);
        require(app.dragTarget_ == MainWindow::CanvasTarget::Track &&
                    app.dragEdges_ == MainWindow::EdgeNone,
                "Thin scrollbar did not expose a move grip");
        app.updatePreviewDrag(canvas, imagePoint(380, 150).x, imagePoint(380, 150).y);
        app.endPreviewDrag(canvas);
        const Rect moved = app.readRect(app.trackEdits_);
        require(moved.width == 1 && moved.x == 380,
                "Thin scrollbar move changed its width or missed the bounds");

        // Candidate changes replace the track, but a crop that the user has
        // touched remains intact. Untouched crops are derived from the track.
        app.setRect(app.viewportEdits_, {17, 19, 101, 121});
        app.markViewportCustomized();
        app.selectScrollbarCandidate(1, false);
        require(MainWindow::sameRect(app.readRect(app.viewportEdits_), {17, 19, 101, 121}) &&
                    MainWindow::sameRect(app.readRect(app.trackEdits_), nearbyTrack()),
                "Candidate switch discarded a manual crop");
        app.viewportCustomized_ = false;
        app.selectScrollbarCandidate(0, false);
        const Rect derived = app.readRect(app.viewportEdits_);
        require(derived.x == 0 && derived.y == 0 && derived.width == 200 &&
                    derived.height == 330,
                "Untouched crop was not derived from the candidate track");
    }

    void testCaptureLossCancelsAndMouseupCommits() {
        resetGestureState();
        setRectangles({0, 0, 190, 330}, {200, 50, 24, 280});
        const Rect original = app.readRect(app.trackEdits_);
        const POINT start = imagePoint(212, 160);
        app.beginPreviewDrag(canvas, start.x, start.y);
        app.updatePreviewDrag(canvas, imagePoint(242, 160).x, imagePoint(242, 160).y);
        require(app.previewInteractionActive(), "Track drag did not become active");
        app.handlePreviewMessage(canvas, WM_CAPTURECHANGED, 0, 0);
        require(!app.previewInteractionActive() &&
                    MainWindow::sameRect(app.readRect(app.trackEdits_), original),
                "Unexpected capture loss did not cancel and restore the drag");

        app.beginPreviewDrag(canvas, start.x, start.y);
        app.updatePreviewDrag(canvas, imagePoint(242, 160).x, imagePoint(242, 160).y);
        app.handlePreviewMessage(canvas, WM_CANCELMODE, 0, 0);
        require(!app.previewInteractionActive() &&
                    MainWindow::sameRect(app.readRect(app.trackEdits_), original),
                "WM_CANCELMODE did not cancel and restore the drag");

        // ReleaseCapture sends WM_CAPTURECHANGED synchronously on Windows.
        // A normal mouseup therefore has to commit before releasing capture.
        app.beginPreviewDrag(canvas, start.x, start.y);
        app.updatePreviewDrag(canvas, imagePoint(242, 160).x, imagePoint(242, 160).y);
        app.handlePreviewMessage(canvas, WM_LBUTTONUP, 0, 0);
        const Rect committed = app.readRect(app.trackEdits_);
        require(committed.x == 230 && committed.width == 24 &&
                    !app.previewInteractionActive(),
                "Normal mouseup released capture before committing the drag");
    }

    void testCandidateUndoPreservesManualTrack() {
        resetGestureState();
        const Rect manual{271, 41, 3, 207};
        setRectangles({0, 0, 190, 330}, manual);
        app.selectedScrollbarCandidate_.reset();
        app.selectScrollbarCandidate(1, false);
        require(app.selectedScrollbarCandidate_ == 1,
                "Candidate chooser did not select the requested candidate");
        app.undoPreviewCalibration();
        require(!app.selectedScrollbarCandidate_ &&
                    MainWindow::sameRect(app.readRect(app.trackEdits_), manual),
                "Undo after candidate selection lost the manual track geometry");
    }

    void testUndoDuringDragDoesNotConsumeHistory() {
        resetGestureState();
        setRectangles({0, 0, 190, 330}, {200, 50, 24, 280});
        const Rect original = app.readRect(app.trackEdits_);
        const POINT start = imagePoint(212, 160);
        app.beginPreviewDrag(canvas, start.x, start.y);
        app.updatePreviewDrag(canvas, imagePoint(232, 160).x, imagePoint(232, 160).y);
        app.endPreviewDrag(canvas);
        const Rect committed = app.readRect(app.trackEdits_);
        const std::size_t completedHistory = app.calibrationHistory_.size();
        require(completedHistory > 0, "Completed drag did not create undo history");

        const int committedCenter = committed.x + committed.width / 2;
        app.beginPreviewDrag(canvas, imagePoint(committedCenter, 160).x,
                             imagePoint(committedCenter, 160).y);
        app.updatePreviewDrag(canvas, imagePoint(committedCenter + 20, 160).x,
                              imagePoint(committedCenter + 20, 160).y);
        require(app.previewInteractionActive(), "Second drag did not become active");
        app.undoPreviewCalibration();
        require(!app.previewInteractionActive() &&
                    MainWindow::sameRect(app.readRect(app.trackEdits_), committed) &&
                    app.calibrationHistory_.size() == completedHistory,
                "Undo during a drag consumed completed history instead of cancelling");

        app.undoPreviewCalibration();
        require(MainWindow::sameRect(app.readRect(app.trackEdits_), original) &&
                    app.calibrationHistory_.size() + 1 == completedHistory,
                "Completed history was not available after cancelling the active drag");
    }

    void testInverseMappingAndGestureBounds() {
        resetGestureState();
        app.fitPreview();
        const auto transform = app.displayTransform(canvas);
        require(transform.has_value(), "Transform missing for inverse mapping test");
        POINT image{};
        require(!app.clientToImage(canvas, transform->destination.left - 1,
                                   transform->destination.top + 20, image),
                "Blank fit margin was accepted as an image point");

        app.setActivePreviewTool(MainWindow::PreviewTool::CaptureArea);
        setRectangles({20, 30, 100, 100}, {350, 30, 1, 250});
        app.startDrawReplacement();
        const POINT blank{transform->destination.left - 1, transform->destination.top + 20};
        app.beginPreviewDrag(canvas, blank.x, blank.y);
        require(!app.gestureActive_ && app.dragTarget_ == MainWindow::CanvasTarget::None,
                "Draw new started from the blank margin");
        app.cancelPreviewGesture(canvas);

        // Once a gesture has started, leaving the client area clamps to the
        // corresponding source edge instead of dropping the mouse update.
        const POINT center = imagePoint(70, 80);
        app.beginPreviewDrag(canvas, center.x, center.y);
        app.updatePreviewDrag(canvas, -1000, -1000);
        app.endPreviewDrag(canvas);
        const Rect clamped = app.readRect(app.viewportEdits_);
        require(clamped.x == 0 && clamped.y == 0 && clamped.width == 100 &&
                    clamped.height == 100,
                "Gesture update outside the canvas did not clamp to source bounds");
    }

    Rect nearbyTrack() const { return app.scrollbarCandidates_[1].config.track; }

    void testThinSideHandleAndDpiLayout() {
        app.fitPreview();
        app.setActivePreviewTool(MainWindow::PreviewTool::Scrollbar);
        setRectangles({0, 0, 190, 330}, {350, 30, 1, 250});
        const auto transform = app.displayTransform(canvas);
        const auto handles = app.previewHandles(app.readRect(app.trackEdits_), *transform);
        const auto right = std::find_if(handles.begin(), handles.end(), [](const auto& h) {
            return h.second == MainWindow::EdgeRight;
        });
        require(right != handles.end(), "Thin track is missing its separate width handle");
        app.beginPreviewDrag(canvas, right->first.x, right->first.y);
        require(app.dragEdges_ == MainWindow::EdgeRight, "Thin track width handle selected a move");
        app.updatePreviewDrag(canvas, right->first.x + 12, right->first.y);
        app.endPreviewDrag(canvas);
        require(app.readRect(app.trackEdits_).width == 13, "Thin track width handle did not widen the track");
        app.previewDpi_ = 192;
        const auto largeHandles = app.previewHandles({350, 30, 1, 250}, *transform);
        require(largeHandles[3].first.x - largeHandles[2].first.x == 72,
                "Precision grips should scale with monitor DPI independently of image zoom");
        app.previewDpi_ = 96;
    }

    static void run() {
        PreviewInteractionTests tests;
        tests.setup();
        try {
            tests.testZoomAnchorAndPan();
            tests.testActiveToolAndNoCandidateSteal();
            tests.testDrawCancelUndoAndBounds();
            tests.testThinTrackAndCropPreservation();
            tests.testThinSideHandleAndDpiLayout();
            tests.testCaptureLossCancelsAndMouseupCommits();
            tests.testCandidateUndoPreservesManualTrack();
            tests.testUndoDuringDragDoesNotConsumeHistory();
            tests.testInverseMappingAndGestureBounds();
            require(tests.app.calibrationHistory_.size() <= MainWindow::kPreviewHistoryLimit,
                    "Calibration history exceeded its bound");
        } catch (...) {
            tests.teardown();
            throw;
        }
        tests.teardown();
    }
};

} // namespace
} // namespace universal_stitcher

int main() {
    try {
        universal_stitcher::PreviewInteractionTests::run();
        std::cout << "Preview interaction tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
