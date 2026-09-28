#pragma once

#include "../app/App.h"

#include <Windows.h>
#include <thread>
#include <atomic>
#include <mutex>

// Forward-declare GDI+ types instead of including <gdiplus.h> here, so the
// (fairly heavy) GDI+ headers only need to be pulled into DebugOverlay.cpp.
namespace Gdiplus { class Graphics; }

namespace fb {

// A borderless, click-through, always-on-top window drawn directly over
// the game near (and, when the platform allows it, ON TOP of) the fishing
// bar.
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
//
// Rendering is per-pixel alpha via UpdateLayeredWindow (not the simpler
// SetLayeredWindowAttributes/color-key mode used originally - color-key
// transparency only supports exact-match-or-fully-opaque per pixel, so a
// blended translucent shape's edges/interior stop being see-through the
// moment they no longer exactly equal the key color; UpdateLayeredWindow
// blends properly against the real desktop at every pixel). Content is
// drawn with GDI+ for anti-aliasing, on a dedicated render thread that
// owns its own DIB section - not through WM_PAINT, which a layered window
// using UpdateLayeredWindow does not receive in the usual way, and not
// through WM_TIMER, whose minimum interval (USER_TIMER_MINIMUM, 10ms) is
// too coarse for a smooth high refresh-rate overlay.
// The overlay's screen position/size, recomputed whenever the ROI's
// screen changes (F4). panelLocal/highlightLocal are WINDOW-LOCAL (i.e.
// relative to windowOriginScreenX/Y) and are the same across screen 1/2
// in the normal case (the whole layout just translates as a unit) -
// windowOriginScreenX/Y is the only part that actually differs.
struct OverlayLayout {
    RECT panelLocal{};
    RECT highlightLocal{};
    int windowOriginScreenX = 0;
    int windowOriginScreenY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
};

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
    void OnDestroy(HWND hwnd);
    void OnClose(HWND hwnd);
    void OnTimer(); // show/hide polling + screen-change (F4) detection; see RenderThreadMain for repaint cadence

    // (Re)computes the layout for the given (possibly screen-2-shifted)
    // ROI. Safe to call again later, not just at Create() time - if the
    // window already exists, it's moved via SetWindowPos to match.
    void RecomputeLayout(const RoiConfig& roi);
    OverlayLayout CurrentLayout() const;

    void RenderThreadMain();

    void Render(Gdiplus::Graphics& g, const UiSnapshot& snap, const OverlayLayout& layout);
    void RenderOnBarHighlights(Gdiplus::Graphics& g, const UiSnapshot& snap, const OverlayLayout& layout); // only if m_captureExcluded
    void RenderSidePanel(Gdiplus::Graphics& g, const UiSnapshot& snap, const OverlayLayout& layout);
    void RenderKeybindHelp(Gdiplus::Graphics& g); // the fixed top-left-of-screen keybind block

    // Screen-space -> this window's client-local pixel conversion.
    static POINT ToLocal(const OverlayLayout& layout, int screenX, int screenY);

    App& m_app;
    HINSTANCE m_hInstance;
    HWND m_hwnd = nullptr;

    // Set by Create() just before each CreateWindowExW call, to whichever
    // of m_hwnd/m_helpHwnd that call is about to fill in. WM_NCCREATE
    // (dispatched synchronously INSIDE CreateWindowExW, before it returns)
    // uses this to assign the real handle early, so that by the time
    // WM_CREATE arrives - also still inside the same CreateWindowExW call -
    // WndProc's `hwnd == m_hwnd` check actually works. Without this, the
    // member CreateWindowExW is about to return into is still null at
    // WM_CREATE time, so that comparison is always false and OnCreate (the
    // SetTimer call the show/hide polling depends on) never runs.
    HWND* m_pendingHwndSlot = nullptr;

    // A second, small, always-fixed-position window (top-left of the
    // primary monitor) listing the hotkeys - shown/hidden together with
    // the main overlay, but never moved by RecomputeLayout/F4, since it
    // isn't tied to the ROI at all.
    HWND m_helpHwnd = nullptr;
    static constexpr int kHelpX = 10;
    static constexpr int kHelpY = 10;
    static constexpr int kHelpWidth = 240;
    static constexpr int kHelpHeight = 176;

    mutable std::mutex m_layoutMutex;
    OverlayLayout m_layout;         // protected by m_layoutMutex - read every render frame, written on F4
    RoiConfig m_lastLayoutRoi{};    // UI-thread only, for OnTimer's screen-change detection

    bool m_captureExcluded = false;
    bool m_visible = false;

    ULONG_PTR m_gdiplusToken = 0;

    std::thread m_renderThread;
    std::atomic<bool> m_renderThreadRunning{false};

    static constexpr UINT_PTR kTimerId = 2;
};

} // namespace fb
