#include "DebugOverlay.h"

#include <objidl.h>
#include <gdiplus.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <chrono>
#include <functional>
#include <memory>

using namespace Gdiplus;

namespace fb {

namespace {
constexpr wchar_t kClassName[] = L"FishingBotDebugOverlay";
constexpr wchar_t kHelpClassName[] = L"FishingBotKeybindHelp";

// ---- Palette -------------------------------------------------------
// A small, cohesive dark palette: near-black slate panel, a soft cyan
// accent for structure/chrome, and semantic colors for marker (red) and
// target (green) that stay consistent everywhere they appear.
const Color kPanelBg(226, 15, 17, 23);
const Color kPanelBorder(90, 120, 190, 210);
const Color kHeaderText(255, 235, 238, 242);
const Color kDivider(50, 255, 255, 255);
const Color kLabelDim(255, 125, 140, 160);
const Color kValueText(255, 205, 210, 220);
const Color kDimText(255, 130, 135, 145);
const Color kMarkerAccent(255, 235, 95, 95);
const Color kMarkerAccentDim(200, 235, 95, 95);
const Color kTrackedAccent(255, 250, 175, 80);
const Color kTargetAccent(255, 90, 220, 140);
const Color kTargetAccentDim(200, 90, 220, 140);
const Color kStatusOn(255, 70, 205, 120);
const Color kStatusOff(255, 210, 85, 85);
const Color kWarnAccent(255, 235, 160, 70);

// GDI+'s GraphicsPath hides its copy constructor (COM-era class, no move
// semantics either), so it can't be returned by value - build it in the
// caller-owned instance instead.
void BuildRoundedRectPath(GraphicsPath& path, const RectF& r, float radius) {
    path.Reset();
    float d = std::min(radius * 2.0f, std::min(r.Width, r.Height));
    if (d <= 0.0f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void FillRoundedRect(Graphics& g, const RectF& r, float radius, const Color& color) {
    SolidBrush brush(color);
    GraphicsPath path;
    BuildRoundedRectPath(path, r, radius);
    g.FillPath(&brush, &path);
}

void DrawRoundedRect(Graphics& g, const RectF& r, float radius, const Color& color, float width) {
    Pen pen(color, width);
    GraphicsPath path;
    BuildRoundedRectPath(path, r, radius);
    g.DrawPath(&pen, &path);
}

std::wstring FormatBand(const wchar_t* label, const DetectionBand& b) {
    std::wostringstream ss;
    ss << label << L"  y=" << (b.present ? b.roiLocalCenterY : -1)
       << L"  c=" << std::fixed << std::setprecision(2) << b.confidence;
    return ss.str();
}

// A small RAII pair: a real DIB section (what UpdateLayeredWindow actually
// reads) plus a GDI+ Bitmap (what GDI+ actually draws into). These must be
// TWO SEPARATE objects: Graphics::Graphics(HDC) - drawing straight onto
// the DIB's device context - does NOT track per-pixel alpha correctly
// (a well-known GDI+ limitation), which silently produced a fully
// transparent result here. Graphics::Graphics(Image*) drawing onto a real
// Bitmap DOES track alpha correctly, so we draw there and copy the result
// into the DIB every frame. Using PixelFormat32bppPARGB also means
// LockBits hands back already-premultiplied data, so no separate
// premultiply pass is needed for UpdateLayeredWindow's AC_SRC_ALPHA.
struct RenderSurface {
    HDC memDc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;
    int width = 0, height = 0;
    int strideBytes = 0;
    std::unique_ptr<Bitmap> gdiBitmap;
    bool valid = false;

    void Create(HDC screenDc, int w, int h) {
        width = w; height = h;
        strideBytes = w * 4;
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        memDc = CreateCompatibleDC(screenDc);
        dib = memDc ? CreateDIBSection(screenDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        gdiBitmap = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        valid = (memDc && dib && bits && gdiBitmap && gdiBitmap->GetLastStatus() == Ok);
        if (memDc && dib) oldBmp = SelectObject(memDc, dib);
    }

    ~RenderSurface() {
        if (memDc && oldBmp) SelectObject(memDc, oldBmp);
        if (dib) DeleteObject(dib);
        if (memDc) DeleteDC(memDc);
    }
};

// Shared by RenderThreadMain for both the main overlay and the small
// keybind help window: draws via GDI+ into the surface's real Bitmap,
// copies the (already-premultiplied) result into its DIB section, and
// presents that at the given screen origin.
void PresentSurface(HWND hwnd, HDC screenDc, RenderSurface& surface, int originX, int originY,
                     const std::function<void(Graphics&)>& draw) {
    {
        Graphics g(surface.gdiBitmap.get());
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.Clear(Color(0, 0, 0, 0));
        draw(g);
    }

    Gdiplus::Rect lockRect(0, 0, surface.width, surface.height);
    Gdiplus::BitmapData bitmapData{};
    if (surface.gdiBitmap->LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppPARGB, &bitmapData) == Ok) {
        const uint8_t* src = static_cast<const uint8_t*>(bitmapData.Scan0);
        uint8_t* dst = static_cast<uint8_t*>(surface.bits);
        int srcStride = bitmapData.Stride; // GDI+ Bitmap rows are top-down here, same as our DIB
        int copyBytes = std::min(std::abs(srcStride), surface.strideBytes);
        for (int y = 0; y < surface.height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * surface.strideBytes,
                        src + static_cast<size_t>(y) * srcStride, static_cast<size_t>(copyBytes));
        }
        surface.gdiBitmap->UnlockBits(&bitmapData);
    }

    POINT srcPoint{0, 0};
    POINT dstPoint{ originX, originY };
    SIZE sz{ surface.width, surface.height };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, screenDc, &dstPoint, &sz, surface.memDc, &srcPoint, 0, &blend, ULW_ALPHA);
}

std::wstring FormatTracked(const TrackedBand& b) {
    std::wostringstream ss;
    if (b.hasData) {
        ss << L"trk  y=" << std::fixed << std::setprecision(0) << b.centerY
           << L"  v=" << std::setprecision(0) << b.velocityPerSec
           << (b.coasting ? L"  *coast*" : L"");
    } else {
        ss << L"trk  no data";
    }
    return ss.str();
}

} // namespace

DebugOverlay::DebugOverlay(App& app, HINSTANCE hInstance) : m_app(app), m_hInstance(hInstance) {
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&m_gdiplusToken, &gdiplusStartupInput, nullptr);
}

DebugOverlay::~DebugOverlay() {
    m_renderThreadRunning.store(false, std::memory_order_relaxed);
    if (m_renderThread.joinable()) m_renderThread.join();
    if (m_helpHwnd) DestroyWindow(m_helpHwnd);
    if (m_hwnd) DestroyWindow(m_hwnd);
    if (m_gdiplusToken) GdiplusShutdown(m_gdiplusToken);
}

LRESULT CALLBACK DebugOverlay::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DebugOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<DebugOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        // See m_pendingHwndSlot's declaration: assign the real handle now,
        // not after CreateWindowExW returns, so WM_CREATE (still to come
        // inside this same call) can tell main window from help window.
        if (self && self->m_pendingHwndSlot) {
            *self->m_pendingHwndSlot = hwnd;
        }
    } else {
        self = reinterpret_cast<DebugOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT DebugOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            // Only the main window has a timer/hotkey-adjacent polling
            // job (show/hide + F4 layout tracking) - the help block is
            // fixed and just needs to exist.
            if (hwnd == m_hwnd) OnCreate(hwnd);
            return 0;
        case WM_TIMER:
            OnTimer();
            return 0;
        case WM_PAINT: {
            // Rendering happens via UpdateLayeredWindow from the render
            // thread, not through the normal WM_PAINT cycle - this is a
            // defensive no-op in case Windows ever sends one anyway.
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_CLOSE:
            OnClose(hwnd);
            return 0;
        case WM_DESTROY:
            OnDestroy(hwnd);
            // Either window being destroyed means the whole app is
            // shutting down (see OnClose) - end the message loop
            // regardless of which one this was.
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool DebugOverlay::Create() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &DebugOverlay::WndProcStatic;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            std::wcerr << L"DebugOverlay RegisterClassExW failed, error=" << err << std::endl;
            return false;
        }
    }

    RoiConfig roi = m_app.CurrentRoiConfig();
    RecomputeLayout(roi); // m_hwnd is still null here, so this only computes m_layout - no SetWindowPos yet
    OverlayLayout layout = CurrentLayout();

    m_pendingHwndSlot = &m_hwnd;
    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"Fishing Bot Debug Overlay", WS_POPUP,
        layout.windowOriginScreenX, layout.windowOriginScreenY,
        layout.windowWidth, layout.windowHeight,
        nullptr, nullptr, m_hInstance, this);

    if (!m_hwnd) {
        std::wcerr << L"DebugOverlay CreateWindowExW failed, error=" << GetLastError() << std::endl;
        return false;
    }

    // No SetLayeredWindowAttributes call: UpdateLayeredWindow (called from
    // the render thread) drives all compositing with true per-pixel alpha
    // instead of exact-match color-key transparency.

    m_captureExcluded = SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE) != 0;
    if (m_captureExcluded) {
        std::wcout << L"[DebugOverlay] capture-excluded on-bar highlights enabled" << std::endl;
    } else {
        std::wcerr << L"[DebugOverlay] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed (error="
                    << GetLastError() << L") - falling back to side-panel-only rendering "
                    << L"so the overlay can never contaminate the capture" << std::endl;
    }

    // The keybind help block: fixed at the primary monitor's top-left
    // corner, unrelated to the ROI, so it needs no hotkeys/timer of its
    // own - but it DOES share WndProcStatic/WndProc with the main window
    // (rather than plain DefWindowProcW) so that a WM_CLOSE landing on
    // this window (e.g. a "main window" heuristic targeting it instead of
    // the status window) still triggers a full, clean app shutdown
    // instead of silently destroying just this one window and leaving the
    // message loop running with nothing left to close it. See WndProc.
    WNDCLASSEXW helpWc{};
    helpWc.cbSize = sizeof(helpWc);
    helpWc.lpfnWndProc = &DebugOverlay::WndProcStatic;
    helpWc.hInstance = m_hInstance;
    helpWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    helpWc.lpszClassName = kHelpClassName;
    if (!RegisterClassExW(&helpWc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            std::wcerr << L"DebugOverlay help-block RegisterClassExW failed, error=" << err << std::endl;
        }
    }
    m_pendingHwndSlot = &m_helpHwnd;
    m_helpHwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kHelpClassName, L"Fishing Bot Keybinds", WS_POPUP,
        kHelpX, kHelpY, kHelpWidth, kHelpHeight,
        nullptr, nullptr, m_hInstance, this);
    if (m_helpHwnd) {
        SetWindowDisplayAffinity(m_helpHwnd, WDA_EXCLUDEFROMCAPTURE);
    } else {
        std::wcerr << L"DebugOverlay help-block CreateWindowExW failed, error=" << GetLastError() << std::endl;
    }

    m_renderThreadRunning.store(true, std::memory_order_relaxed);
    m_renderThread = std::thread([this] { RenderThreadMain(); });

    return true;
}

void DebugOverlay::OnCreate(HWND hwnd) {
    SetTimer(hwnd, kTimerId, static_cast<UINT>(std::max(30, m_app.UiRefreshIntervalMs())), nullptr);
}

void DebugOverlay::OnDestroy(HWND hwnd) {
    if (hwnd == m_hwnd) {
        KillTimer(m_hwnd, kTimerId);
        m_hwnd = nullptr; // avoid a stale-handle DestroyWindow call from our own destructor
    } else if (hwnd == m_helpHwnd) {
        m_helpHwnd = nullptr;
    }
}

void DebugOverlay::OnClose(HWND hwnd) {
    // This window is click-through/no-activate and normally never receives
    // a close request directly, but some external tools (or a "main
    // window" heuristic) can still target it instead of the status
    // window. Treat it as a full app shutdown rather than silently
    // destroying just this window.
    m_app.RequestExit();
    DestroyWindow(hwnd);
}

void DebugOverlay::OnTimer() {
    bool shouldShow = m_app.IsDebugModeOn();
    if (shouldShow != m_visible) {
        m_visible = shouldShow;
        ShowWindow(m_hwnd, m_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (m_helpHwnd) {
            ShowWindow(m_helpHwnd, m_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
    }

    // F4 (App::ToggleScreen) can move the ROI to a different monitor at
    // any time; reposition the overlay to follow it. This piggybacks on
    // the existing show/hide poll rather than adding another timer -
    // hotkey-triggered, so sub-150ms latency isn't needed.
    RoiConfig roiNow = m_app.CurrentRoiConfig();
    if (roiNow.screenX != m_lastLayoutRoi.screenX || roiNow.screenY != m_lastLayoutRoi.screenY) {
        RecomputeLayout(roiNow);
    }
}

void DebugOverlay::RecomputeLayout(const RoiConfig& roi) {
    const int margin = 6;
    RECT highlightScreen{
        roi.screenX - roi.flankPixels - margin,
        roi.screenY - margin,
        roi.screenX + roi.width + roi.flankPixels + margin,
        roi.screenY + roi.height + margin
    };

    const int panelWidth = 260;
    const int panelGap = 14;
    const int panelHeight = std::max(300, roi.height + 40);
    RECT panelScreen{
        highlightScreen.left - panelGap - panelWidth,
        roi.screenY - 10,
        highlightScreen.left - panelGap,
        roi.screenY - 10 + panelHeight
    };

    RECT unionRect{
        std::min(panelScreen.left, highlightScreen.left) - 4,
        std::min(panelScreen.top, highlightScreen.top) - 4,
        std::max(panelScreen.right, highlightScreen.right) + 4,
        std::max(panelScreen.bottom, highlightScreen.bottom) + 4
    };

    int vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    int shiftX = 0, shiftY = 0;
    if (unionRect.left < vLeft) shiftX = vLeft - unionRect.left;
    if (unionRect.top < vTop) shiftY = vTop - unionRect.top;
    if (unionRect.right + shiftX > vLeft + vWidth) shiftX -= (unionRect.right + shiftX) - (vLeft + vWidth);
    if (unionRect.bottom + shiftY > vTop + vHeight) shiftY -= (unionRect.bottom + shiftY) - (vTop + vHeight);
    if (shiftX != 0 || shiftY != 0) {
        OffsetRect(&unionRect, shiftX, shiftY);
        OffsetRect(&panelScreen, shiftX, shiftY);
        OffsetRect(&highlightScreen, shiftX, shiftY);
    }

    OverlayLayout newLayout;
    newLayout.windowOriginScreenX = unionRect.left;
    newLayout.windowOriginScreenY = unionRect.top;
    newLayout.windowWidth = unionRect.right - unionRect.left;
    newLayout.windowHeight = unionRect.bottom - unionRect.top;
    newLayout.panelLocal = RECT{
        panelScreen.left - unionRect.left, panelScreen.top - unionRect.top,
        panelScreen.right - unionRect.left, panelScreen.bottom - unionRect.top
    };
    newLayout.highlightLocal = RECT{
        highlightScreen.left - unionRect.left, highlightScreen.top - unionRect.top,
        highlightScreen.right - unionRect.left, highlightScreen.bottom - unionRect.top
    };

    {
        std::lock_guard<std::mutex> lock(m_layoutMutex);
        m_layout = newLayout;
    }
    m_lastLayoutRoi = roi;

    // Only reposition an already-created window - during the initial call
    // from Create(), m_hwnd is still null and CreateWindowExW is given the
    // layout directly instead. Width/height are expected to stay constant
    // across a screen toggle (see OverlayLayout) - the render thread's DIB
    // is sized once at startup and is NOT reallocated if that assumption
    // ever doesn't hold (e.g. a second monitor with a very different
    // resolution pushing the virtual-screen clamp differently); only the
    // window's position would still update correctly in that case.
    if (m_hwnd) {
        SetWindowPos(m_hwnd, nullptr, newLayout.windowOriginScreenX, newLayout.windowOriginScreenY,
                     newLayout.windowWidth, newLayout.windowHeight, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

OverlayLayout DebugOverlay::CurrentLayout() const {
    std::lock_guard<std::mutex> lock(m_layoutMutex);
    return m_layout;
}

POINT DebugOverlay::ToLocal(const OverlayLayout& layout, int screenX, int screenY) {
    return POINT{ screenX - layout.windowOriginScreenX, screenY - layout.windowOriginScreenY };
}

void DebugOverlay::RenderThreadMain() {
    using ClockT = std::chrono::steady_clock;

    // Width/height are fixed for the life of this thread - only the main
    // window's screen position (read fresh every frame below) changes
    // when F4 moves the ROI to a different monitor. See RecomputeLayout.
    // The help block's size/position are fixed for the whole run.
    OverlayLayout initialLayout = CurrentLayout();
    if (initialLayout.windowWidth <= 0 || initialLayout.windowHeight <= 0) return;

    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return;

    RenderSurface mainSurface;
    mainSurface.Create(screenDc, initialLayout.windowWidth, initialLayout.windowHeight);
    if (!mainSurface.valid) {
        ReleaseDC(nullptr, screenDc);
        std::wcerr << L"[DebugOverlay] render surface allocation failed - overlay will not draw" << std::endl;
        return;
    }

    RenderSurface helpSurface;
    if (m_helpHwnd) {
        helpSurface.Create(screenDc, kHelpWidth, kHelpHeight);
    }

    while (m_renderThreadRunning.load(std::memory_order_relaxed)) {
        auto frameStart = ClockT::now();

        if (m_app.IsDebugModeOn()) {
            UiSnapshot snap = m_app.GetSnapshot();
            OverlayLayout layout = CurrentLayout(); // re-read every frame - F4 can change the origin at any time

            PresentSurface(m_hwnd, screenDc, mainSurface,
                            layout.windowOriginScreenX, layout.windowOriginScreenY,
                            [&](Graphics& g) { Render(g, snap, layout); });

            if (m_helpHwnd && helpSurface.valid) {
                PresentSurface(m_helpHwnd, screenDc, helpSurface,
                                kHelpX, kHelpY,
                                [&](Graphics& g) { RenderKeybindHelp(g); });
            }
        }

        float hz = m_app.OverlayRenderHz();
        double intervalSec = (hz > 0.0f) ? (1.0 / static_cast<double>(hz)) : (1.0 / 60.0);
        auto target = frameStart + std::chrono::duration_cast<ClockT::duration>(std::chrono::duration<double>(intervalSec));
        auto now = ClockT::now();
        if (target > now) {
            std::this_thread::sleep_until(target);
        }
    }

    ReleaseDC(nullptr, screenDc);
}

void DebugOverlay::Render(Graphics& g, const UiSnapshot& snap, const OverlayLayout& layout) {
    if (m_captureExcluded) {
        RenderOnBarHighlights(g, snap, layout);
    }
    RenderSidePanel(g, snap, layout);
}

void DebugOverlay::RenderOnBarHighlights(Graphics& g, const UiSnapshot& snap, const OverlayLayout& layout) {
    const RoiConfig& roi = snap.roi; // already reflects the current screen (App::UpdateSnapshot uses CurrentRoiConfig())

    // Frame around the real ROI + flank, drawn just outside it. Already
    // window-local (see RecomputeLayout) - no screen conversion needed.
    {
        const RECT& hl = layout.highlightLocal;
        RectF r(static_cast<float>(hl.left), static_cast<float>(hl.top),
                static_cast<float>(hl.right - hl.left), static_cast<float>(hl.bottom - hl.top));
        DrawRoundedRect(g, r, 10.0f, Color(210, 70, 230, 150), 2.0f);
    }

    // Detected target band, drawn as a genuinely translucent fill directly
    // over the real bar at its true screen position (true per-pixel alpha
    // now - this shows the game content underneath, not a muted opaque
    // approximation).
    if (snap.lastDetection.target.present) {
        int topScreenY = RoiLocalYToScreenY(roi, snap.lastDetection.target.roiLocalTop);
        int botScreenY = RoiLocalYToScreenY(roi, snap.lastDetection.target.roiLocalBottom);
        POINT tl = ToLocal(layout, roi.screenX, topScreenY);
        POINT br = ToLocal(layout, roi.screenX + roi.width, botScreenY);
        LONG heightPx = std::max<LONG>(br.y - tl.y, 2);
        RectF r(static_cast<float>(tl.x), static_cast<float>(tl.y),
                static_cast<float>(br.x - tl.x), static_cast<float>(heightPx));
        FillRoundedRect(g, r, 4.0f, Color(100, 80, 225, 130));
        DrawRoundedRect(g, r, 4.0f, Color(230, 90, 235, 150), 1.6f);
    }

    // Raw marker position, drawn as a bright bar across the ROI width.
    if (snap.lastDetection.marker.present) {
        int centerScreenY = RoiLocalYToScreenY(roi, snap.lastDetection.marker.roiLocalCenterY);
        POINT c1 = ToLocal(layout, roi.screenX, centerScreenY - 2);
        POINT c2 = ToLocal(layout, roi.screenX + roi.width, centerScreenY + 2);
        RectF r(static_cast<float>(c1.x), static_cast<float>(c1.y),
                static_cast<float>(c2.x - c1.x), static_cast<float>(c2.y - c1.y));
        FillRoundedRect(g, r, 2.0f, Color(245, 245, 245, 245));
        DrawRoundedRect(g, r, 2.0f, kMarkerAccent, 1.4f);
    }
}

void DebugOverlay::RenderSidePanel(Graphics& g, const UiSnapshot& snap, const OverlayLayout& layout) {
    const RECT& pl = layout.panelLocal;
    RectF panel(static_cast<float>(pl.left), static_cast<float>(pl.top),
                static_cast<float>(pl.right - pl.left), static_cast<float>(pl.bottom - pl.top));

    FillRoundedRect(g, panel, 12.0f, kPanelBg);
    DrawRoundedRect(g, panel, 12.0f, kPanelBorder, 1.2f);

    FontFamily segoe(L"Segoe UI");
    FontFamily consolas(L"Consolas");
    Font headerFont(&segoe, 13.0f, FontStyleBold, UnitPixel);
    Font pillFont(&segoe, 9.0f, FontStyleBold, UnitPixel);
    Font labelFont(&segoe, 9.0f, FontStyleBold, UnitPixel);
    Font dataFont(&consolas, 12.0f, FontStyleRegular, UnitPixel);
    Font smallFont(&consolas, 10.0f, FontStyleRegular, UnitPixel);

    StringFormat centerFmt;
    centerFmt.SetAlignment(StringAlignmentCenter);
    centerFmt.SetLineAlignment(StringAlignmentCenter);

    const float pad = 14.0f;
    float x = panel.X + pad;
    float y = panel.Y + pad;

    // ---- Header: title + ON/OFF status pill ----
    SolidBrush headerBrush(kHeaderText);
    g.DrawString(L"FISHING BOT", -1, &headerFont, PointF(x, y), &headerBrush);

    {
        RectF pill(panel.X + panel.Width - pad - 46.0f, y - 1.0f, 46.0f, 18.0f);
        FillRoundedRect(g, pill, 9.0f, snap.enabled ? kStatusOn : kStatusOff);
        SolidBrush pillText(Color(255, 20, 20, 22));
        const wchar_t* pillLabel = snap.enabled ? L"ON" : L"OFF";
        g.DrawString(pillLabel, -1, &pillFont, pill, &centerFmt, &pillText);
    }
    y += 24.0f;

    // ---- Divider ----
    Pen dividerPen(kDivider, 1.0f);
    g.DrawLine(&dividerPen, x, y, panel.X + panel.Width - pad, y);
    y += 10.0f;

    // ---- State ----
    std::wostringstream stateSs;
    stateSs << ToString(snap.state) << L"  -  " << std::fixed << std::setprecision(0)
            << snap.stateElapsedMs << L"ms";
    Color stateColor = kValueText;
    switch (snap.state) {
        case BotState::Fishing: stateColor = kTargetAccent; break;
        case BotState::WaitT: stateColor = kWarnAccent; break;
        default: break;
    }
    SolidBrush stateBrush(stateColor);
    g.DrawString(stateSs.str().c_str(), -1, &dataFont, PointF(x, y), &stateBrush);
    y += 22.0f;

    // ---- Marker section ----
    {
        SolidBrush dotBrush(kMarkerAccent);
        g.FillEllipse(&dotBrush, x, y + 3.0f, 6.0f, 6.0f);
        SolidBrush labelBrush(kLabelDim);
        g.DrawString(L"MARKER", -1, &labelFont, PointF(x + 12.0f, y), &labelBrush);
        y += 15.0f;

        SolidBrush rawBrush(kMarkerAccent);
        g.DrawString(FormatBand(L"raw", snap.lastDetection.marker).c_str(), -1, &dataFont, PointF(x, y), &rawBrush);
        y += 17.0f;
        SolidBrush trkBrush(kTrackedAccent);
        g.DrawString(FormatTracked(snap.trackedMarker).c_str(), -1, &dataFont, PointF(x, y), &trkBrush);
        y += 22.0f;
    }

    // ---- Target section ----
    {
        SolidBrush dotBrush(kTargetAccent);
        g.FillEllipse(&dotBrush, x, y + 3.0f, 6.0f, 6.0f);
        SolidBrush labelBrush(kLabelDim);
        g.DrawString(L"TARGET", -1, &labelFont, PointF(x + 12.0f, y), &labelBrush);
        y += 15.0f;

        SolidBrush rawBrush(kTargetAccent);
        g.DrawString(FormatBand(L"raw", snap.lastDetection.target).c_str(), -1, &dataFont, PointF(x, y), &rawBrush);
        y += 17.0f;
        SolidBrush trkBrush(kTrackedAccent);
        g.DrawString(FormatTracked(snap.trackedTarget).c_str(), -1, &dataFont, PointF(x, y), &trkBrush);
        y += 19.0f;

        // In-zone/out-of-zone color axis reading: a small gradient bar
        // (matching the game's own in-zone green -> out-of-zone yellow)
        // with a pointer at the current reading - ground truth from the
        // game's own UI, shown only when the winning match's color was
        // close enough to the axis to trust (see Detector.cpp).
        if (snap.lastDetection.targetInZoneColorFraction >= 0.0f) {
            float f = std::clamp(snap.lastDetection.targetInZoneColorFraction, 0.0f, 1.0f);
            RectF barRect(x, y, panel.Width - 2.0f * pad, 7.0f);
            Color gradStart(255, 0x51, 0xDE, 0x09);
            Color gradEnd(255, 0xDA, 0xC8, 0x09);
            LinearGradientBrush gradBrush(barRect, gradStart, gradEnd, LinearGradientModeHorizontal);
            FillRoundedRect(g, barRect, 3.5f, Color(255, 40, 40, 40));
            GraphicsPath gradPath;
            BuildRoundedRectPath(gradPath, barRect, 3.5f);
            g.FillPath(&gradBrush, &gradPath);

            float pointerX = barRect.X + f * barRect.Width;
            SolidBrush pointerBrush(Color(255, 255, 255, 255));
            Pen pointerPen(Color(255, 20, 20, 20), 1.2f);
            PointF tri[3] = {
                PointF(pointerX, barRect.Y - 2.0f),
                PointF(pointerX - 4.0f, barRect.Y - 8.0f),
                PointF(pointerX + 4.0f, barRect.Y - 8.0f)
            };
            g.FillPolygon(&pointerBrush, tri, 3);
            g.DrawPolygon(&pointerPen, tri, 3);
            y += 14.0f;
        }
        y += 8.0f;
    }

    // ---- Input state pills ----
    {
        auto drawPill = [&](float px, const wchar_t* text, bool active) {
            RectF pill(px, y, 62.0f, 18.0f);
            FillRoundedRect(g, pill, 9.0f, active ? Color(255, 70, 150, 220) : Color(140, 55, 58, 66));
            SolidBrush textBrush(active ? Color(255, 15, 20, 30) : kDimText);
            g.DrawString(text, -1, &pillFont, pill, &centerFmt, &textBrush);
        };
        drawPill(x, snap.mouseHeld ? L"M1 DOWN" : L"M1 up", snap.mouseHeld);
        drawPill(x + 70.0f, snap.tHeld ? L"T DOWN" : L"T up", snap.tHeld);
        y += 26.0f;
    }

    // ---- Performance ----
    {
        std::wostringstream fpsSs;
        fpsSs << L"cap " << std::fixed << std::setprecision(0) << snap.captureFps
              << L"fps   det " << snap.detectionFps << L"fps";
        SolidBrush dimBrush(kDimText);
        g.DrawString(fpsSs.str().c_str(), -1, &smallFont, PointF(x, y), &dimBrush);
        y += 16.0f;
        if (!m_captureExcluded) {
            SolidBrush warnBrush(kWarnAccent);
            g.DrawString(L"on-bar highlights disabled: no", -1, &smallFont, PointF(x, y), &warnBrush);
            y += 13.0f;
            g.DrawString(L"WDA_EXCLUDEFROMCAPTURE", -1, &smallFont, PointF(x, y), &warnBrush);
            y += 16.0f;
        }
        if (!snap.statusMessage.empty()) {
            SolidBrush statusBrush(kWarnAccent);
            std::wstring msg = L"! " + snap.statusMessage;
            RectF msgRect(x, y, panel.Width - 2.0f * pad, 40.0f);
            g.DrawString(msg.c_str(), -1, &smallFont, msgRect, nullptr, &statusBrush);
            y += 28.0f;
        }
        y += 6.0f;
    }

    // ---- Bar schematic + marker evidence histogram ----
    // Named barLayout, distinct from the `layout` (OverlayLayout) function
    // parameter - this is the little schematic's own internal DebugBarLayout.
    const RoiConfig& roi = snap.roi;
    DebugBarLayout barLayout;
    barLayout.debugLeft = static_cast<int>(x) + 6;
    barLayout.debugTop = static_cast<int>(y);
    barLayout.debugWidth = 32;
    barLayout.debugHeight = static_cast<int>(panel.Y + panel.Height - pad - y);
    if (barLayout.debugHeight < 24 || roi.height <= 0) return;

    RectF schematicRect(static_cast<float>(barLayout.debugLeft), static_cast<float>(barLayout.debugTop),
                         static_cast<float>(barLayout.debugWidth), static_cast<float>(barLayout.debugHeight));
    DrawRoundedRect(g, schematicRect, 6.0f, Color(150, 150, 155, 165), 1.2f);

    // Percentage gridlines as a scale reference.
    {
        Pen gridPen(Color(90, 90, 95, 105), 1.0f);
        gridPen.SetDashStyle(DashStyleDot);
        SolidBrush gridText(Color(160, 130, 135, 145));
        for (int pct = 0; pct <= 100; pct += 25) {
            int roiLocalY = (roi.height - 1) * pct / 100;
            int debugY = RoiLocalYToDebugY(roi, barLayout, roiLocalY);
            g.DrawLine(&gridPen, static_cast<float>(barLayout.debugLeft), static_cast<float>(debugY),
                       static_cast<float>(barLayout.debugLeft + barLayout.debugWidth), static_cast<float>(debugY));
            std::wostringstream lbl;
            lbl << pct << L"%";
            g.DrawString(lbl.str().c_str(), -1, &smallFont,
                         PointF(static_cast<float>(barLayout.debugLeft) - 30.0f, static_cast<float>(debugY) - 7.0f), &gridText);
        }
    }

    // Marker evidence strip: fraction of columns classified marker-like
    // per row - still genuine per-pixel evidence (target no longer has an
    // equivalent, since it's detected structurally rather than by
    // per-pixel classification - see Detector.cpp).
    bool haveMarkerMask = snap.debugMode
        && snap.lastDetection.maskWidth == roi.width
        && snap.lastDetection.maskHeight == roi.height
        && static_cast<int>(snap.lastDetection.markerMask.size()) == roi.width * roi.height;

    if (haveMarkerMask) {
        int evidenceX = barLayout.debugLeft + barLayout.debugWidth + 10;
        int evidenceWidth = 16;
        SolidBrush evidenceBrush(kMarkerAccentDim);
        for (int roiLocalY = 0; roiLocalY < roi.height; ++roiLocalY) {
            int markerTrue = 0;
            const uint8_t* mRow = &snap.lastDetection.markerMask[static_cast<size_t>(roiLocalY) * roi.width];
            for (int cx = 0; cx < roi.width; ++cx) if (mRow[cx]) ++markerTrue;
            float frac = static_cast<float>(markerTrue) / static_cast<float>(roi.width);

            int y0 = RoiLocalYToDebugY(roi, barLayout, roiLocalY);
            int y1 = RoiLocalYToDebugY(roi, barLayout, roiLocalY + 1);
            if (y1 <= y0) y1 = y0 + 1;
            float w = frac * evidenceWidth;
            if (w > 0.5f) {
                g.FillRectangle(&evidenceBrush, static_cast<float>(evidenceX), static_cast<float>(y0), w, static_cast<float>(y1 - y0));
            }
        }
    }

    // Detected target band (raw), green rounded outline.
    if (snap.lastDetection.target.present) {
        int topY = RoiLocalYToDebugY(roi, barLayout, snap.lastDetection.target.roiLocalTop);
        int botY = RoiLocalYToDebugY(roi, barLayout, snap.lastDetection.target.roiLocalBottom);
        if (botY <= topY) botY = topY + 1;
        RectF tRect(static_cast<float>(barLayout.debugLeft), static_cast<float>(topY),
                    static_cast<float>(barLayout.debugWidth), static_cast<float>(botY - topY));
        FillRoundedRect(g, tRect, 3.0f, Color(70, 90, 220, 140));
        DrawRoundedRect(g, tRect, 3.0f, kTargetAccent, 1.6f);
    }

    // Raw marker center - red line.
    if (snap.lastDetection.marker.present) {
        int markerY = RoiLocalYToDebugY(roi, barLayout, snap.lastDetection.marker.roiLocalCenterY);
        Pen markerPen(kMarkerAccent, 2.0f);
        g.DrawLine(&markerPen, static_cast<float>(barLayout.debugLeft - 6), static_cast<float>(markerY),
                   static_cast<float>(barLayout.debugLeft + barLayout.debugWidth + 6), static_cast<float>(markerY));
    }

    // Tracked/predicted marker - dashed orange line.
    if (snap.trackedMarker.hasData) {
        int roiLocalYPred = static_cast<int>(std::lround(snap.trackedMarker.centerY));
        int predictedY = RoiLocalYToDebugY(roi, barLayout, roiLocalYPred);
        Pen predPen(kTrackedAccent, 1.6f);
        float dash[2] = {4.0f, 3.0f};
        predPen.SetDashPattern(dash, 2);
        g.DrawLine(&predPen, static_cast<float>(barLayout.debugLeft - 6), static_cast<float>(predictedY),
                   static_cast<float>(barLayout.debugLeft + barLayout.debugWidth + 6), static_cast<float>(predictedY));
    }
}

void DebugOverlay::RenderKeybindHelp(Graphics& g) {
    RectF panel(0.0f, 0.0f, static_cast<float>(kHelpWidth), static_cast<float>(kHelpHeight));
    FillRoundedRect(g, panel, 10.0f, kPanelBg);
    DrawRoundedRect(g, panel, 10.0f, kPanelBorder, 1.2f);

    FontFamily segoe(L"Segoe UI");
    FontFamily consolas(L"Consolas");
    Font headerFont(&segoe, 12.0f, FontStyleBold, UnitPixel);
    Font keyFont(&consolas, 12.0f, FontStyleBold, UnitPixel);
    Font descFont(&segoe, 11.0f, FontStyleRegular, UnitPixel);

    const float pad = 12.0f;
    float x = pad;
    float y = pad;
    const float lineHeight = 22.0f;

    SolidBrush headerBrush(kHeaderText);
    g.DrawString(L"KEYBINDS", -1, &headerFont, PointF(x, y), &headerBrush);
    y += lineHeight;

    Pen dividerPen(kDivider, 1.0f);
    g.DrawLine(&dividerPen, x, y - 4.0f, panel.Width - pad, y - 4.0f);

    struct KeyRow { const wchar_t* key; const wchar_t* desc; };
    static const KeyRow kRows[] = {
        { L"F1", L"toggle bot" },
        { L"F2", L"exit" },
        { L"F3", L"toggle debug overlay" },
        { L"F4", L"switch screen (1/2)" },
        { L"H+6", L"Sell Runo" },
        { L"G+6", L"Buy Fish Head" },
        { L"H+7", L"Sell Shiro" },
    };

    SolidBrush keyBrush(kTargetAccent);
    SolidBrush descBrush(kValueText);
    for (const KeyRow& row : kRows) {
        g.DrawString(row.key, -1, &keyFont, PointF(x, y), &keyBrush);
        g.DrawString(row.desc, -1, &descFont, PointF(x + 40.0f, y + 1.0f), &descBrush);
        y += lineHeight;
    }
}

} // namespace fb
