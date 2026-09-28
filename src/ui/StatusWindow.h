#pragma once

#include "../app/App.h"
#include "MacroConfigPanel.h"

#include <Windows.h>

namespace fb {

// The main control window: a compact text readout (state, timers, ROI,
// detection values, input state, fps) tab, and one settings tab per macro
// sequence (per-step delay/duration, see MacroConfigPanel) - plus the
// F1-F4 global hotkeys (the macro sequences use H+6/G+6/H+7 key chords
// instead - see App::CheckMacroHotkeys). The visual debug schematic
// itself is drawn by DebugOverlay, positioned over the game near the
// real fishing bar - see ui/DebugOverlay.h.
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
    void OnNotify(LPARAM lParam);
    void OnCommand(WPARAM wParam);
    void OnTabChanged();

    void Render(HDC hdc, const RECT& clientRect, const UiSnapshot& snap);
    void RenderStatusText(HDC hdc, int x, int y, const UiSnapshot& snap);

    App& m_app;
    HINSTANCE m_hInstance;
    HWND m_hwnd = nullptr;
    HWND m_tabControl = nullptr;
    MacroConfigPanel m_macroPanelA;
    MacroConfigPanel m_macroPanelB;
    MacroConfigPanel m_macroPanelC;
    int m_activeTab = 0; // 0 = Status, 1 = Sell Runo, 2 = Buy Fish Head, 3 = Sell Shiro

    static constexpr int kHotkeyToggleId = 1;
    static constexpr int kHotkeyExitId = 2;
    static constexpr int kHotkeyDebugId = 3;
    static constexpr int kHotkeyScreenId = 4;
    // F5/kHotkeyMacroId removed - the macro sequences are now triggered by
    // the H+6 / G+6 / H+7 key chords, polled directly inside App (see
    // App::CheckMacroHotkeys), not a RegisterHotKey binding.
    static constexpr UINT_PTR kTimerId = 1;
    static constexpr int kTabControlId = 100;
    static constexpr int kMacroPanelAIdBase = 200; // reserves [200,299) for panel A's controls
    static constexpr int kMacroPanelBIdBase = 300; // reserves [300,399) for panel B's controls
    static constexpr int kMacroPanelCIdBase = 400; // reserves [400,499) for panel C's controls

    // Client-area layout: the tab strip sits at the top, everything else
    // (status text or a macro panel, depending on the selected tab) below it.
    static constexpr int kTabStripHeight = 26;
    static constexpr int kContentTop = kTabStripHeight + 8;
};

} // namespace fb
