#include "X11Capture.h"

#include <sys/ipc.h>
#include <sys/shm.h>

namespace fb {

X11Capture::~X11Capture() {
    Shutdown();
}

bool X11Capture::Initialize(const RoiConfig& roi, std::string& outError) {
    Shutdown();

    m_display = XOpenDisplay(nullptr);
    if (!m_display) {
        outError = "XOpenDisplay failed - is $DISPLAY set?";
        return false;
    }
    m_root = DefaultRootWindow(m_display);
    m_useShm = XShmQueryExtension(m_display) != 0;

    if (!EnsureImageBuffer(roi.CaptureWidth(), roi.CaptureHeight())) {
        outError = "Failed to allocate the capture buffer (MIT-SHM setup failed and the fallback also failed)";
        Shutdown();
        return false;
    }

    m_lastError.clear();
    return true;
}

bool X11Capture::EnsureImageBuffer(int width, int height) {
    if (m_imageWidth == width && m_imageHeight == height && (m_shmImage || !m_useShm)) {
        return true; // already sized correctly
    }
    DestroyImageBuffer();

    if (m_useShm) {
        int screen = DefaultScreen(m_display);
        m_shmImage = XShmCreateImage(m_display, DefaultVisual(m_display, screen),
                                      static_cast<unsigned int>(DefaultDepth(m_display, screen)),
                                      ZPixmap, nullptr, &m_shmInfo, width, height);
        if (m_shmImage) {
            m_shmInfo.shmid = shmget(IPC_PRIVATE,
                                      static_cast<size_t>(m_shmImage->bytes_per_line) * static_cast<size_t>(m_shmImage->height),
                                      IPC_CREAT | 0600);
            if (m_shmInfo.shmid < 0) {
                XDestroyImage(m_shmImage);
                m_shmImage = nullptr;
            } else {
                m_shmInfo.shmaddr = m_shmImage->data = static_cast<char*>(shmat(m_shmInfo.shmid, nullptr, 0));
                m_shmInfo.readOnly = False;
                if (m_shmInfo.shmaddr == reinterpret_cast<char*>(-1) || !XShmAttach(m_display, &m_shmInfo)) {
                    if (m_shmInfo.shmaddr != reinterpret_cast<char*>(-1)) shmdt(m_shmInfo.shmaddr);
                    shmctl(m_shmInfo.shmid, IPC_RMID, nullptr);
                    XDestroyImage(m_shmImage);
                    m_shmImage = nullptr;
                }
            }
        }
        if (!m_shmImage) {
            // MIT-SHM setup failed for some reason (e.g. no shared memory
            // permission in a sandboxed/containerized environment) - fall
            // back to plain XGetImage rather than failing outright.
            m_useShm = false;
        }
    }

    m_imageWidth = width;
    m_imageHeight = height;
    return true; // the no-SHM path needs no persistent buffer - XGetImage builds one per call
}

void X11Capture::DestroyImageBuffer() {
    if (m_shmImage) {
        XShmDetach(m_display, &m_shmInfo);
        shmdt(m_shmInfo.shmaddr);
        shmctl(m_shmInfo.shmid, IPC_RMID, nullptr);
        XDestroyImage(m_shmImage); // frees the XImage struct itself, not the (already-detached) shm segment
        m_shmImage = nullptr;
    }
}

void X11Capture::Shutdown() {
    DestroyImageBuffer();
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
    m_imageWidth = 0;
    m_imageHeight = 0;
}

CaptureStatus X11Capture::AcquireFrame(const RoiConfig& roi, CapturedFrame& outFrame, int /*timeoutMs*/) {
    if (!m_display) {
        m_lastError = "Not initialized";
        return CaptureStatus::Failed;
    }

    int width = roi.CaptureWidth();
    int height = roi.CaptureHeight();
    int originX = roi.CaptureOriginScreenX();
    int originY = roi.CaptureOriginScreenY();

    if (!EnsureImageBuffer(width, height)) {
        m_lastError = "Failed to (re)allocate the capture buffer";
        return CaptureStatus::Failed;
    }

    XImage* image = nullptr;
    bool ownsImage = false;

    if (m_useShm && m_shmImage) {
        if (!XShmGetImage(m_display, m_root, m_shmImage, originX, originY, AllPlanes)) {
            m_lastError = "XShmGetImage failed";
            return CaptureStatus::Failed;
        }
        image = m_shmImage;
    } else {
        image = XGetImage(m_display, m_root, originX, originY, width, height, AllPlanes, ZPixmap);
        if (!image) {
            m_lastError = "XGetImage failed";
            return CaptureStatus::Failed;
        }
        ownsImage = true;
    }

    outFrame.frameId = ++m_frameCounter;
    outFrame.timestamp = Clock::now();
    outFrame.width = width;
    outFrame.height = height;
    outFrame.captureOriginScreenX = originX;
    outFrame.captureOriginScreenY = originY;

    // Normalized to tightly-packed BGRA regardless of the X server's actual
    // pixel layout - CapturedFrame's contract (see common/Types.h) is a
    // BGRA buffer, matching the Windows capture's output format exactly,
    // so the shared Detector code never needs to know which OS produced
    // the frame. XGetPixel is used for correctness (it handles whatever
    // bit depth/byte order the X server actually uses) rather than a
    // hand-rolled fast path that could silently assume the wrong layout
    // on a system this was never tested against.
    outFrame.strideBytes = width * 4;
    outFrame.pixelsBgra.assign(static_cast<size_t>(outFrame.strideBytes) * static_cast<size_t>(height), 0);

    for (int y = 0; y < height; ++y) {
        uint8_t* dstRow = outFrame.pixelsBgra.data() + static_cast<size_t>(y) * static_cast<size_t>(outFrame.strideBytes);
        for (int x = 0; x < width; ++x) {
            unsigned long pixel = XGetPixel(image, x, y);
            uint8_t r = static_cast<uint8_t>((pixel & image->red_mask) >> 16);
            uint8_t g = static_cast<uint8_t>((pixel & image->green_mask) >> 8);
            uint8_t b = static_cast<uint8_t>(pixel & image->blue_mask);
            dstRow[x * 4 + 0] = b;
            dstRow[x * 4 + 1] = g;
            dstRow[x * 4 + 2] = r;
            dstRow[x * 4 + 3] = 255;
        }
    }

    if (ownsImage) XDestroyImage(image);
    return CaptureStatus::Ok;
}

} // namespace fb
