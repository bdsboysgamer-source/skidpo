#include "DebugOverlay.h"

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace fb {

namespace {
constexpr wchar_t kClassName[] = L"FishingBotDebugOverlay";

void DrawTranslucentRect(HDC hdc, RECT r, COLORREF color, BYTE alpha) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, bmp);

    RECT local{0, 0, w, h};
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(memDc, &local, brush);
    DeleteObject(brush);

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat = 0;
    AlphaBlend(hdc, r.left, r.top, w, h, memDc, 0, 0, w, h, bf);

    SelectObject(memDc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(memDc);
}
} // namespace

DebugOverlay::DebugOverlay(App& app, HINSTANCE hInstance) : m_app(app), m_hInstance(hInstance) {}

DebugOverlay::~DebugOverlay() {
    if (m_hwnd) DestroyWindow(m_hwnd);
}

LRESULT CALLBACK DebugOverlay::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DebugOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<DebugOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DebugOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT DebugOverlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;
        case WM_TIMER:
            OnTimer();
            return 0;
        case WM_PAINT:
            OnPaint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            // This window is click-through/no-activate and normally never
            // receives a close request directly, but some external tools
            // (or a "main window" heuristic) can still target it instead
            // of the status window. Treat it the same as closing the
            // status window - a full app shutdown - rather than silently
            // destroying just this window and leaving the rest running
            // headless with the message loop never seeing WM_QUIT.
            OnClose(hwnd);
            return 0;
        case WM_DESTROY:
            OnDestroy();
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

    RoiConfig roi = m_app.GetRoiConfig();
    const int margin = 6;
    m_highlightScreenRect = {
        roi.screenX - roi.flankPixels - margin,
        roi.screenY - margin,
        roi.screenX + roi.width + roi.flankPixels + margin,
        roi.screenY + roi.height + margin
    };

    const int panelWidth = 250;
    const int panelGap = 14;
    const int panelHeight = std::max(280, roi.height + 20);
    m_panelScreenRect = {
        m_highlightScreenRect.left - panelGap - panelWidth,
        roi.screenY - 10,
        m_highlightScreenRect.left - panelGap,
        roi.screenY - 10 + panelHeight
    };

    RECT unionRect{
        std::min(m_panelScreenRect.left, m_highlightScreenRect.left) - 4,
        std::min(m_panelScreenRect.top, m_highlightScreenRect.top) - 4,
        std::max(m_panelScreenRect.right, m_highlightScreenRect.right) + 4,
        std::max(m_panelScreenRect.bottom, m_highlightScreenRect.bottom) + 4
    };

    // Keep the overlay on the virtual desktop even if the ROI sits near an edge.
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
        OffsetRect(&m_panelScreenRect, shiftX, shiftY);
        OffsetRect(&m_highlightScreenRect, shiftX, shiftY);
    }
    m_windowScreenRect = unionRect;

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kClassName, L"Fishing Bot Debug Overlay", WS_POPUP,
        m_windowScreenRect.left, m_windowScreenRect.top,
        m_windowScreenRect.right - m_windowScreenRect.left,
        m_windowScreenRect.bottom - m_windowScreenRect.top,
        nullptr, nullptr, m_hInstance, this);

    if (!m_hwnd) {
        std::wcerr << L"DebugOverlay CreateWindowExW failed, error=" << GetLastError() << std::endl;
        return false;
    }

    SetLayeredWindowAttributes(m_hwnd, kColorKey, 0, LWA_COLORKEY);

    // The whole reason this overlay is allowed to sit visually on top of
    // the real fishing bar: exclude it from every capture API (including
    // our own Desktop Duplication capture), so there is no feedback path
    // from the overlay back into the detector.
    m_captureExcluded = SetWindowDisplayAffinity(m_hwnd, WDA_EXCLUDEFROMCAPTURE) != 0;
    if (m_captureExcluded) {
        std::wcout << L"[DebugOverlay] capture-excluded on-bar highlights enabled" << std::endl;
    } else {
        std::wcerr << L"[DebugOverlay] SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed (error="
                    << GetLastError() << L") - falling back to side-panel-only rendering "
                    << L"so the overlay can never contaminate the capture" << std::endl;
    }

    return true;
}

void DebugOverlay::OnCreate(HWND hwnd) {
    SetTimer(hwnd, kTimerId, static_cast<UINT>(std::max(30, m_app.UiRefreshIntervalMs())), nullptr);
}

void DebugOverlay::OnDestroy() {
    KillTimer(m_hwnd, kTimerId);
    m_hwnd = nullptr; // avoid a stale-handle DestroyWindow call from our own destructor
}

void DebugOverlay::OnClose(HWND hwnd) {
    m_app.RequestExit();
    DestroyWindow(hwnd);
}

void DebugOverlay::OnTimer() {
    bool shouldShow = m_app.GetSnapshot().debugMode;
    if (shouldShow != m_visible) {
        m_visible = shouldShow;
        ShowWindow(m_hwnd, m_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    if (m_visible) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

POINT DebugOverlay::ToLocal(int screenX, int screenY) const {
    return POINT{ screenX - m_windowScreenRect.left, screenY - m_windowScreenRect.top };
}

void DebugOverlay::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);

    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    HBRUSH keyBrush = CreateSolidBrush(kColorKey);
    FillRect(memDc, &clientRect, keyBrush);
    DeleteObject(keyBrush);

    UiSnapshot snap = m_app.GetSnapshot();
    Render(memDc, snap);

    BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);

    EndPaint(hwnd, &ps);
}

void DebugOverlay::Render(HDC hdc, const UiSnapshot& snap) {
    SetBkMode(hdc, TRANSPARENT);
    HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(hdc, font);

    if (m_captureExcluded) {
        RenderOnBarHighlights(hdc, snap);
    }
    RenderSidePanel(hdc, snap);

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void DebugOverlay::RenderOnBarHighlights(HDC hdc, const UiSnapshot& snap) {
    const RoiConfig& roi = snap.roi;

    // Frame around the real ROI + flank, drawn just outside it, matching
    // the reference tool's highlight box around the live fishing bar.
    {
        POINT tl = ToLocal(m_highlightScreenRect.left, m_highlightScreenRect.top);
        POINT br = ToLocal(m_highlightScreenRect.right, m_highlightScreenRect.bottom);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(70, 230, 120));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, tl.x, tl.y, br.x, br.y);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    // Detected target band, drawn as a translucent highlight directly over
    // the real bar at its true screen position.
    if (snap.lastDetection.target.present) {
        int topScreenY = RoiLocalYToScreenY(roi, snap.lastDetection.target.roiLocalTop);
        int botScreenY = RoiLocalYToScreenY(roi, snap.lastDetection.target.roiLocalBottom);
        POINT tl = ToLocal(roi.screenX, topScreenY);
        POINT br = ToLocal(roi.screenX + roi.width, botScreenY);
        RECT r{ tl.x, tl.y, br.x, std::max(br.y, tl.y + 2) };
        DrawTranslucentRect(hdc, r, RGB(80, 220, 100), 110);
    }

    // Raw marker position, drawn as a bright bar across the ROI width.
    if (snap.lastDetection.marker.present) {
        int centerScreenY = RoiLocalYToScreenY(roi, snap.lastDetection.marker.roiLocalCenterY);
        POINT c1 = ToLocal(roi.screenX, centerScreenY - 2);
        POINT c2 = ToLocal(roi.screenX + roi.width, centerScreenY + 2);
        RECT r{ c1.x, c1.y, c2.x, c2.y };
        HBRUSH brush = CreateSolidBrush(RGB(245, 245, 245));
        FillRect(hdc, &r, brush);
        DeleteObject(brush);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(235, 60, 60));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, r.left, r.top, r.right, r.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
}

void DebugOverlay::RenderSidePanel(HDC hdc, const UiSnapshot& snap) {
    POINT tl = ToLocal(m_panelScreenRect.left, m_panelScreenRect.top);
    POINT br = ToLocal(m_panelScreenRect.right, m_panelScreenRect.bottom);
    RECT panel{ tl.x, tl.y, br.x, br.y };

    // Opaque backing so panel text is legible over any game content.
    DrawTranslucentRect(hdc, panel, RGB(10, 10, 14), 210);
    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 100));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, panel.left, panel.top, panel.right, panel.bottom);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    const int lineHeight = 15;
    int x = panel.left + 8;
    int y = panel.top + 6;
    int line = 0;
    auto put = [&](const std::wstring& text, COLORREF color) {
        SetTextColor(hdc, color);
        TextOutW(hdc, x, y + line * lineHeight, text.c_str(), static_cast<int>(text.size()));
        ++line;
    };

    put(snap.enabled ? L"BOT: ON" : L"BOT: OFF", snap.enabled ? RGB(90, 230, 110) : RGB(220, 100, 100));
    std::wostringstream stateSs;
    stateSs << L"State: " << ToString(snap.state) << L" (" << std::fixed << std::setprecision(0)
            << snap.stateElapsedMs << L"ms)";
    put(stateSs.str(), RGB(220, 220, 130));

    std::wostringstream mSs;
    mSs << L"M raw y=" << (snap.lastDetection.marker.present ? snap.lastDetection.marker.roiLocalCenterY : -1)
        << L" c=" << std::setprecision(2) << snap.lastDetection.marker.confidence;
    put(mSs.str(), RGB(235, 90, 90));

    std::wostringstream mtSs;
    mtSs << L"M trk y=" << std::setprecision(0) << (snap.trackedMarker.hasData ? snap.trackedMarker.centerY : -1.0f)
         << L" v=" << std::setprecision(0) << snap.trackedMarker.velocityPerSec
         << (snap.trackedMarker.coasting ? L" *coast*" : L"");
    put(mtSs.str(), RGB(255, 165, 60));

    std::wostringstream zSs;
    zSs << L"Z raw t=" << (snap.lastDetection.target.present ? snap.lastDetection.target.roiLocalTop : -1)
        << L" b=" << (snap.lastDetection.target.present ? snap.lastDetection.target.roiLocalBottom : -1)
        << L" c=" << std::setprecision(2) << snap.lastDetection.target.confidence;
    put(zSs.str(), RGB(90, 220, 130));

    if (snap.lastDetection.targetInZoneColorFraction >= 0.0f) {
        float f = snap.lastDetection.targetInZoneColorFraction; // 0=in-zone color, 1=out-of-zone color
        std::wostringstream axisSs;
        axisSs << L"Z axis=" << std::fixed << std::setprecision(2) << f << (f <= 0.5f ? L" (in)" : L" (out)");
        BYTE r = static_cast<BYTE>(81 + (218 - 81) * std::min(1.0f, std::max(0.0f, f)));
        BYTE g = static_cast<BYTE>(222 + (200 - 222) * std::min(1.0f, std::max(0.0f, f)));
        put(axisSs.str(), RGB(r, g, 20));
    }

    std::wostringstream inSs;
    inSs << L"Mouse:" << (snap.mouseHeld ? L"DOWN" : L"up") << L"  T:" << (snap.tHeld ? L"DOWN" : L"up");
    put(inSs.str(), RGB(220, 220, 220));

    std::wostringstream fpsSs;
    fpsSs << L"cap=" << std::setprecision(0) << snap.captureFps << L"fps det=" << snap.detectionFps << L"fps";
    put(fpsSs.str(), RGB(160, 160, 160));
    if (!m_captureExcluded) {
        put(L"(on-bar hl. disabled: no", RGB(200, 130, 60));
        put(L" WDA_EXCLUDEFROMCAPTURE)", RGB(200, 130, 60));
    }
    ++line;

    // Compact bar schematic + per-row evidence histogram, mirroring the
    // reference tool's small graph. All coordinates below are converted
    // through RoiLocalYToDebugY() against a layout local to this panel -
    // never reused against the on-bar screen-space transform above.
    const RoiConfig& roi = snap.roi;
    DebugBarLayout layout;
    layout.debugLeft = panel.left + 10;
    layout.debugTop = panel.top + 6 + line * lineHeight + 8;
    layout.debugWidth = 34;
    layout.debugHeight = panel.bottom - layout.debugTop - 10;
    if (layout.debugHeight < 20 || roi.height <= 0) return;

    {
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(140, 140, 140));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, layout.debugLeft, layout.debugTop, layout.debugLeft + layout.debugWidth,
                  layout.debugTop + layout.debugHeight);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    bool haveMasks = snap.debugMode
                    && snap.lastDetection.maskWidth == roi.width
                    && snap.lastDetection.maskHeight == roi.height
                    && static_cast<int>(snap.lastDetection.targetMask.size()) == roi.width * roi.height
                    && static_cast<int>(snap.lastDetection.markerMask.size()) == roi.width * roi.height;

    const int evidenceWidth = 16;
    int targetEvidenceX = layout.debugLeft + layout.debugWidth + 8;
    int markerEvidenceX = targetEvidenceX + evidenceWidth + 6;

    if (haveMasks) {
        HBRUSH targetBrush = CreateSolidBrush(RGB(50, 180, 80));
        HBRUSH markerBrush = CreateSolidBrush(RGB(210, 170, 40));
        for (int roiLocalY = 0; roiLocalY < roi.height; ++roiLocalY) {
            int targetTrue = 0, markerTrue = 0;
            const uint8_t* tRow = &snap.lastDetection.targetMask[static_cast<size_t>(roiLocalY) * roi.width];
            const uint8_t* mRow = &snap.lastDetection.markerMask[static_cast<size_t>(roiLocalY) * roi.width];
            for (int cx = 0; cx < roi.width; ++cx) {
                if (tRow[cx]) ++targetTrue;
                if (mRow[cx]) ++markerTrue;
            }
            float targetFrac = static_cast<float>(targetTrue) / static_cast<float>(roi.width);
            float markerFrac = static_cast<float>(markerTrue) / static_cast<float>(roi.width);

            int y0 = RoiLocalYToDebugY(roi, layout, roiLocalY);
            int y1 = RoiLocalYToDebugY(roi, layout, roiLocalY + 1);
            if (y1 <= y0) y1 = y0 + 1;

            int tw = static_cast<int>(targetFrac * evidenceWidth);
            if (tw > 0) { RECT r{ targetEvidenceX, y0, targetEvidenceX + tw, y1 }; FillRect(hdc, &r, targetBrush); }
            int mw = static_cast<int>(markerFrac * evidenceWidth);
            if (mw > 0) { RECT r{ markerEvidenceX, y0, markerEvidenceX + mw, y1 }; FillRect(hdc, &r, markerBrush); }
        }
        DeleteObject(targetBrush);
        DeleteObject(markerBrush);
    }

    if (snap.lastDetection.target.present) {
        int topY = RoiLocalYToDebugY(roi, layout, snap.lastDetection.target.roiLocalTop);
        int botY = RoiLocalYToDebugY(roi, layout, snap.lastDetection.target.roiLocalBottom);
        if (botY <= topY) botY = topY + 1;
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(50, 230, 110));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, layout.debugLeft, topY, layout.debugLeft + layout.debugWidth, botY);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    if (snap.lastDetection.marker.present) {
        int markerY = RoiLocalYToDebugY(roi, layout, snap.lastDetection.marker.roiLocalCenterY);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(235, 50, 50));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, layout.debugLeft - 6, markerY, nullptr);
        LineTo(hdc, layout.debugLeft + layout.debugWidth + 6, markerY);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }

    if (snap.trackedMarker.hasData) {
        int roiLocalYPred = static_cast<int>(std::lround(snap.trackedMarker.centerY));
        int predictedY = RoiLocalYToDebugY(roi, layout, roiLocalYPred);
        HPEN pen = CreatePen(PS_DASH, 2, RGB(255, 165, 40));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        MoveToEx(hdc, layout.debugLeft - 6, predictedY, nullptr);
        LineTo(hdc, layout.debugLeft + layout.debugWidth + 6, predictedY);
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
}

} // namespace fb
