# Universal Scroll Stitcher

[![Build portable Windows release](https://github.com/Shaderx/UniversalScrollStitcher/actions/workflows/build.yml/badge.svg)](https://github.com/Shaderx/UniversalScrollStitcher/actions/workflows/build.yml)

Universal Scroll Stitcher is a lightweight Windows desktop utility that turns a manually scrolled window into one continuous PNG or JPEG. It is content-agnostic: there are no templates, OCR rules, process hooks, simulated wheel events, or input injection.

## Download

Download the latest `UniversalScrollStitcher-windows-x64.zip` from [GitHub Releases](https://github.com/Shaderx/UniversalScrollStitcher/releases/latest), extract it anywhere, and run `UniversalScrollStitcher.exe`. The release is portable and requires no installer or separately installed OpenCV/Visual C++ runtime. Third-party license notices are included in the package.

Windows 10 or 11 x64 is required. Some protected or hardware-overlay surfaces cannot be captured by Windows capture APIs.

## How it works

The stitcher combines two signals:

- The scrollbar thumb establishes movement, direction, top/bottom state, and a displacement estimate.
- Frame-to-frame pixel registration finds the exact vertical displacement and rejects ambiguous matches.
- A low-difference row inside the overlap becomes the seam.
- Accepted strips are written to temporary disk-backed storage, avoiding an ever-growing screenshot in RAM.

The user always scrolls manually. The app only observes captured frames and never sends input to the target window.

## Use

1. Select a top-level target window. Click **Refresh** if it was opened after the stitcher.
2. Click **Capture preview**. The preview scales to the app window and highlights distinct scrollbar candidates near both edges.
3. Click the correct numbered candidate to make it the orange selected scrollbar. The green rectangle is the content viewport. Drag inside either rectangle to move it, or drag an edge to resize it. The viewport should exclude fixed window chrome and the scrollbar.
4. Click **Start**, switch to the target, and manually scroll downward. Prefer small mouse-wheel notches or the down arrow; large wheel bursts can jump past the visible overlap. Starting mid-document is allowed; scroll back to the top first only when you want a full-page capture.
5. After the last content is visible, click **Stop**, then **Export PNG/JPEG**.

If a scroll briefly loses visual overlap, the status shows **RECOVERING** and the session stays live: scroll back up slightly until the overlap returns. Only a sustained failure pauses the session so you can stop and export the valid prefix. Overlay scrollbars that fade while idle are treated the same way and do not end the capture.

### Diagnostic logging

The footer contains an **Enable diagnostic logging** checkbox, which is off by
default, and an **Open logs folder** button. When enabled, the app writes a
timestamped UTF-8 session log to
`%LOCALAPPDATA%\UniversalScrollStitcher\logs`. If that location is not
writable, it uses the Windows temporary directory instead. The status panel
shows the exact active log path.

Logs contain process/session context and capture diagnostics such as target
window metadata, calibration rectangles, scrollbar candidates, source and
frame dimensions, stitch confidence, state changes, and export results. They
never contain screenshot pixels or image contents. Files are flushed after
every entry, capped and rolled over at approximately 2 MiB, and retained to
the newest 10 session files. Logging failures are non-fatal and are reported
in the status panel. The modern light UI keeps the logging controls and the
latest portable-release credit visible in the footer.

## Portable release pipeline

The GitHub Actions workflow builds and tests a statically linked x64 Release executable. Each successful push to `main` uploads a `UniversalScrollStitcher-windows-x64` artifact and automatically publishes its ZIP as the latest GitHub Release under a `build-N` tag. Pushing a version tag such as `v0.1.0` publishes a named release instead; tags containing a suffix such as `v0.1.0-rc.1` are marked as prereleases.

To make the same portable folder locally:

```powershell
cmake -S . -B build-portable -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
cmake --build build-portable --config Release
ctest --test-dir build-portable -C Release --output-on-failure
cmake --install build-portable --config Release --prefix release-candidate
```

The generated `release-candidate/` folder is intentionally ignored by Git
because GitHub Actions recreates it from a clean build. The exact static x64
configuration links the OpenCV runtime into the executable, so the folder is
self-contained and does not need OpenCV or Visual C++ runtime DLLs installed
on the destination PC. It also contains third-party license notices; copy the
folder to another compatible Windows x64 PC and run the executable without an
installer.

## Developer build

Requirements:

- Visual Studio 2022 with the Desktop development with C++ workload
- Windows 10/11 SDK
- CMake 3.24 or newer
- vcpkg

Dependencies are declared in [`vcpkg.json`](vcpkg.json). If C++/WinRT headers are present, Windows Graphics Capture can capture a window while it is partly occluded. Otherwise, the app falls back to visible-window GDI capture.

## Current limitations

- One vertical, downward-scrolling viewport per session
- The viewport size must remain constant during capture
- Large wheel jumps that outrun the visible overlap need a slower notch or a small upward scroll to recover; sustained loss pauses the session
- Animated, parallax, rapidly changing, zoomed, or horizontally scrolling content may not stitch reliably
- Custom scrollbars with little contrast may require manual track adjustment
- JPEG exports taller than the Windows codec limit (~65,000 rows) are split into numbered parts; PNG stays a single file unless the capture is extraordinarily tall; PNG is preferred for UI text
