// Exercise the actual preview handlers with hidden Win32 controls.
#include "../src/main.cpp"
#include <iostream>
#include <stdexcept>

namespace universal_stitcher {
namespace {
struct PreviewInteractionTests {
    static void run() {
        MainWindow app;
        HWND canvas = CreateWindowExW(0, L"STATIC", L"", WS_POPUP,
                                      0, 0, 416, 416, nullptr, nullptr, nullptr, nullptr);
        if (!canvas) throw std::runtime_error("Cannot create test canvas");
        app.previewCanvas_ = canvas;
        for (auto* edits : {&app.trackEdits_, &app.viewportEdits_}) {
            for (auto& edit : *edits) {
                edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD,
                                       0, 0, 40, 20, canvas, nullptr, nullptr, nullptr);
                if (!edit) throw std::runtime_error("Cannot create test edit");
            }
        }
        app.preview_ = cv::Mat(400, 400, CV_8UC4);
        ScrollbarCandidate selected{}, nearby{};
        selected.config.track = {200, 50, 24, 280};
        nearby.config.track = {228, 50, 24, 280};
        app.scrollbarCandidates_ = {selected, nearby};
        app.selectedScrollbarCandidate_ = 0;
        app.setRect(app.trackEdits_, selected.config.track);
        app.setRect(app.viewportEdits_, {0, 0, 190, 330});
        // The selected right edge also falls in the neighboring candidate's
        // hit radius. It must start a resize, not select that candidate.
        app.beginPreviewDrag(canvas, 224 + 8, 150 + 8);
        if (app.selectedScrollbarCandidate_ != 0 ||
            app.dragTarget_ != MainWindow::CanvasTarget::Track) {
            throw std::runtime_error("Resize switched away from the selected scrollbar");
        }
        app.updatePreviewDrag(canvas, 244 + 8, 150 + 8);
        app.endPreviewDrag(canvas);
        if (app.readRect(app.trackEdits_).width != 44 ||
            app.selectedScrollbarCandidate_ != 0 || app.readRect(app.viewportEdits_).width != 190) {
            throw std::runtime_error("Resize did not preserve the selected track and viewport");
        }
        // Moving the track across a detected candidate keeps the drag target.
        app.setRect(app.trackEdits_, {200, 50, 40, 280});
        app.beginPreviewDrag(canvas, 220 + 8, 150 + 8);
        app.updatePreviewDrag(canvas, 240 + 8, 160 + 8);
        app.endPreviewDrag(canvas);
        if (app.readRect(app.trackEdits_).x != 220 || app.selectedScrollbarCandidate_ != 0) {
            throw std::runtime_error("Move switched scrollbar selection");
        }
        // A viewport edge overlapping another candidate is editable too.
        app.setRect(app.trackEdits_, {300, 50, 24, 280});
        app.setRect(app.viewportEdits_, {0, 0, 228, 330});
        app.beginPreviewDrag(canvas, 228 + 8, 150 + 8);
        app.updatePreviewDrag(canvas, 218 + 8, 150 + 8);
        app.endPreviewDrag(canvas);
        if (app.readRect(app.viewportEdits_).width != 218 || app.selectedScrollbarCandidate_ != 0) {
            throw std::runtime_error("Viewport resize switched scrollbar selection");
        }
        // Candidate picking still works outside both editable rectangles.
        app.beginPreviewDrag(canvas, 240 + 8, 150 + 8);
        if (app.selectedScrollbarCandidate_ != 1 || app.dragTarget_ != MainWindow::CanvasTarget::None) {
            throw std::runtime_error("Explicit candidate picking stopped working");
        }
        DestroyWindow(canvas);
    }
};
}
}

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
