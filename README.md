# Universal Scroll Stitcher

Universal Scroll Stitcher is a small Windows desktop utility for making one long screenshot while the user manually scrolls a window. It is intentionally content-agnostic: there are no game templates, OCR rules, process hooks, simulated wheel events, or input injection.

The stitcher uses a hybrid signal:

* the scrollbar thumb establishes whether the view moved, the scroll direction, top/bottom state, and a displacement prior;
* frame-to-frame pixel registration finds the exact vertical displacement and rejects ambiguous matches;
* a low-difference row inside the overlap is selected as the seam;
* accepted strips are written to a temporary disk-backed store so intermediate full screenshots do not accumulate in RAM.

## Build

The supported build is Visual Studio 2022 (v143) with a Windows 10/11 SDK and CMake 3.24 or newer:

```powershell
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

If C++/WinRT headers are available (for example through a vcpkg `cppwinrt` package), the app uses Windows Graphics Capture and can capture a window even when it is partly occluded. Without those headers the same UI falls back to a visible-window GDI capture. The fallback is useful for development but cannot capture minimized or protected surfaces.

## Use

1. Select a top-level target window and click **Refresh** if it was opened after launch.
2. Click **Capture preview** once. The app fills the client size and attempts to locate a scrollbar near either edge.
3. Check or adjust the viewport and scrollbar track fields. Coordinates are pixels in the captured image; the viewport should exclude fixed chrome and the scrollbar. The track rectangle should cover the scrollbar track, not just the thumb.
4. Click **Start** and manually scroll down in small increments. The app never sends input to the target window.
5. Stop after the last content is visible, then choose **Export PNG/JPEG**.

When the scrollbar moves but no reliable visual overlap can be found, the session pauses and reports a gap. Slow down and continue only after starting a new session; the MVP deliberately does not guess across a missing overlap. A scrollbar that changes appearance or disappears can be handled by changing the track fields and starting again.

## Algorithm notes and limitations

The current MVP assumes a single vertically scrolling viewport whose width and height remain constant. It supports downward scrolling only. It is not reliable for animated/parallax content, rapidly changing lists, horizontal scrolling, zoom changes, window resizes, or a scroll gesture larger than one viewport. The detector is heuristic and works best when the scrollbar thumb has a different luminance from its track; the manual track controls are the fallback for custom scrollbars.

PNG is preferred for UI text. JPEG export exposes a quality setting in code and automatically splits images taller than the JPEG 65,535-pixel limit into numbered parts. Temporary raw strips are removed after a successful session finalization or when the process exits normally.

The repository is intentionally independent from `UmaUmaChecker`; it does not modify or link the parent checkout.
