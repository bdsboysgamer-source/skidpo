#pragma once

#include "common/Types.h"

#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>

#include <string>

namespace fb {

enum class CaptureStatus {
    Ok,          // a new frame was captured into outFrame
    NoNewFrame,  // not currently produced by this backend (X11 capture is
                 // synchronous/on-demand, unlike DXGI's duplicate-frame
                 // signaling) - kept for interface parity with the Windows
                 // DesktopDuplication so App's capture loop needs no
                 // extra branches.
    DeviceLost,  // not currently produced by this backend; kept for the
                 // same reason as NoNewFrame.
    Failed,      // unrecoverable-ish failure; see LastError()
};

// X11 counterpart to DesktopDuplication.h on Windows: captures a fixed
// absolute-screen-space sub-rectangle (the ROI + background flank) - the
// SAME capture model as the Windows version (a screen region, not a
// specific application window). Uses the MIT-SHM extension when available
// for speed, falling back to plain XGetImage otherwise.
class X11Capture {
public:
    X11Capture() = default;
    ~X11Capture();

    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    // Opens the X11 display connection ($DISPLAY) and allocates the
    // capture buffer for roi's size.
    bool Initialize(const RoiConfig& roi, std::string& outError);

    void Shutdown();
    bool IsInitialized() const { return m_display != nullptr; }

    // Captures the roi.CaptureWidth() x roi.CaptureHeight() sub-rect
    // (screen-space origin roi.CaptureOriginScreenX/Y) into outFrame.
    // timeoutMs is accepted for interface parity with the Windows version
    // but unused - X11 capture here is synchronous, not event-driven.
    CaptureStatus AcquireFrame(const RoiConfig& roi, CapturedFrame& outFrame, int timeoutMs);

    const std::string& LastError() const { return m_lastError; }

private:
    bool EnsureImageBuffer(int width, int height);
    void DestroyImageBuffer();

    Display* m_display = nullptr;
    Window m_root = 0;
    bool m_useShm = false;

    XShmSegmentInfo m_shmInfo{};
    XImage* m_shmImage = nullptr; // only used when m_useShm
    int m_imageWidth = 0;
    int m_imageHeight = 0;

    std::string m_lastError;
    uint64_t m_frameCounter = 0;
};

} // namespace fb
