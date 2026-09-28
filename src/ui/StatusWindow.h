#pragma once

#include "../app/App.h"

#include <Windows.h>

namespace fb {

// The main control window: a compact text readout (state, timers, ROI,
// detection values, input state, fps) and the F1-F4 global hotkeys (the
// macro sequences use M+S+1/M+S+B chords instead - see App::CheckMacroHotkeys).
// The visual debug schematic itself is drawn by DebugOverlay, positioned
// over the game near the real fishing bar - see ui/DebugOverlay.h.
class StatusWindow {
public:
    StatusWindow(App& app, HINSTANCE hInstance);
    ~StatusWindow();

    StatusWindow(const StatusWindow&) = delete;
    StatusWindow& operator=(const StatusWindow&) = delete;

    bool Create(int nCmdShow);
    HWND Handle() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void OnCreate(HWND hwnd);
    void OnDestroy();
    void OnHotkey(int id);
    void OnTimer();
    void OnPaint(HWND hwnd);

    void Render(HDC hdc, const RECT& clientRect, const UiSnapshot& snap);
    void RenderStatusText(HDC hdc, int x, int y, const UiSnapshot& snap);

    App& m_app;
    HINSTANCE m_hInstance;
    HWND m_hwnd = nullptr;

    static constexpr int kHotkeyToggleId = 1;
    static constexpr int kHotkeyExitId = 2;
    static constexpr int kHotkeyDebugId = 3;
    static constexpr int kHotkeyScreenId = 4;
    // F5/kHotkeyMacroId removed - the macro sequences are now triggered by
    // the M+S+1 / M+S+B key chords, polled directly inside App (see
    // App::CheckMacroHotkeys), not a RegisterHotKey binding.
    static constexpr UINT_PTR kTimerId = 1;
};

} // namespace fb
