#pragma once

#include "../app/App.h"

#include <Windows.h>

namespace fb {

// A borderless, click-through, always-on-top window drawn directly over
// the game near (and, when the platform allows it, ON TOP of) the fishing
// bar - the same visual style as the reference overlay tool.
//
// The correctness hazard with drawing on top of the ROI is that DXGI
// Desktop Duplication captures the composed desktop, which would normally
// include our own overlay pixels and feed them right back into the
// detector. We avoid that with SetWindowDisplayAffinity(WDA_EXCLUDE-
// FROMCAPTURE): the window stays visible to the human eye but is excluded
// from every screen-capture API, Desktop Duplication included. We verify
// that call actually succeeded at runtime; if it ever doesn't (older
// Windows build, policy restriction, etc.) this class falls back to
// drawing nothing inside the real ROI+flank rectangle - only the side
// panel - so a capture-contamination bug can never silently reappear.
class DebugOverlay {
public:
    DebugOverlay(App& app, HINSTANCE hInstance);
    ~DebugOverlay();

    DebugOverlay(const DebugOverlay&) = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;

    bool Create();
    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnDestroy();
    void OnClose(HWND hwnd);
    void OnTimer();
    void OnPaint(HWND hwnd);

    void Render(HDC hdc, const UiSnapshot& snap);
    void RenderOnBarHighlights(HDC hdc, const UiSnapshot& snap); // only if m_captureExcluded
    void RenderSidePanel(HDC hdc, const UiSnapshot& snap);

    // Screen-space -> this window's client-local pixel conversion.
    POINT ToLocal(int screenX, int screenY) const;

    App& m_app;
    HINSTANCE m_hInstance;
    HWND m_hwnd = nullptr;

    RECT m_windowScreenRect{};   // this overlay window's own bounds, in screen space
    RECT m_panelScreenRect{};    // side info-panel area, in screen space
    RECT m_highlightScreenRect{}; // frame drawn around the ROI+flank, in screen space

    bool m_captureExcluded = false;
    bool m_visible = false;

    static constexpr UINT_PTR kTimerId = 2;
    static constexpr COLORREF kColorKey = RGB(1, 2, 3); // "transparent" marker color
};

} // namespace fb
