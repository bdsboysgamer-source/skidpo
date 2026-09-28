#include "StatusWindow.h"

#include <commctrl.h>

#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <iostream>

namespace fb {

namespace {
constexpr wchar_t kClassName[] = L"FishingBotStatusWindow";

std::wstring FormatBand(const wchar_t* label, const DetectionBand& b) {
    std::wostringstream ss;
    ss << label << L": ";
    if (b.present) {
        ss << L"present top=" << b.roiLocalTop << L" bottom=" << b.roiLocalBottom
           << L" center=" << b.roiLocalCenterY << L" conf=" << std::fixed << std::setprecision(2) << b.confidence
           << L" cols=" << b.supportingColumns;
    } else {
        ss << L"ABSENT";
    }
    return ss.str();
}

std::wstring FormatTracked(const wchar_t* label, const TrackedBand& b) {
    std::wostringstream ss;
    ss << label << L": ";
    if (b.hasData) {
        ss << L"center=" << std::fixed << std::setprecision(1) << b.centerY
           << L" vel=" << std::setprecision(1) << b.velocityPerSec
           << L" conf=" << std::setprecision(2) << b.confidence
           << (b.coasting ? L" [coasting]" : L"");
    } else {
        ss << L"no data";
    }
    return ss.str();
}
} // namespace

StatusWindow::StatusWindow(App& app, HINSTANCE hInstance)
    : m_app(app), m_hInstance(hInstance)
    , m_macroPanelA(app, App::MacroId::A), m_macroPanelB(app, App::MacroId::B), m_macroPanelC(app, App::MacroId::C) {}

StatusWindow::~StatusWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
}

LRESULT CALLBACK StatusWindow::WndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    StatusWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = reinterpret_cast<StatusWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<StatusWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->WndProc(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT StatusWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            OnCreate(hwnd);
            return 0;
        case WM_HOTKEY:
            OnHotkey(static_cast<int>(wParam));
            return 0;
        case WM_TIMER:
            OnTimer();
            return 0;
        case WM_PAINT:
            OnPaint(hwnd);
            return 0;
        case WM_NOTIFY:
            OnNotify(lParam);
            return 0;
        case WM_COMMAND:
            OnCommand(wParam);
            return 0;
        case WM_ERASEBKGND:
            return 1; // avoid flicker; we paint the whole client area ourselves
        case WM_CLOSE:
            m_app.RequestExit();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            OnDestroy();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool StatusWindow::Create(int nCmdShow) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &StatusWindow::WndProcStatic;
    wc.hInstance = m_hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            std::wcerr << L"RegisterClassExW failed, error=" << err << std::endl;
            return false;
        }
    }

    // Sized from a desired CLIENT area (not the outer window) via
    // AdjustWindowRect, since the macro config tabs need a known, fairly
    // wide/tall client area to lay their rows out in (see MacroConfigPanel);
    // 600x460 comfortably fits the longer macro (11 rows) plus the tab strip.
    RECT desiredClient{0, 0, 600, 460};
    AdjustWindowRect(&desiredClient, WS_OVERLAPPEDWINDOW, FALSE);
    int outerW = desiredClient.right - desiredClient.left;
    int outerH = desiredClient.bottom - desiredClient.top;

    // WS_CLIPCHILDREN: without it, this window's own WM_PAINT (full-client
    // black fill + status text, re-triggered by every ~150ms timer tick)
    // paints straight over the tab control/macro panel child windows'
    // pixels, which then have to redraw on top a moment later - a
    // continuous visible flicker once there were real child controls to
    // flicker over.
    m_hwnd = CreateWindowExW(0, kClassName, L"Fishing Bot", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, outerW, outerH,
                              nullptr, nullptr, m_hInstance, this);
    if (!m_hwnd) {
        std::wcerr << L"CreateWindowExW failed, error=" << GetLastError() << std::endl;
        return false;
    }

    ShowWindow(m_hwnd, nCmdShow);
    UpdateWindow(m_hwnd);
    return true;
}

void StatusWindow::OnCreate(HWND hwnd) {
    if (!RegisterHotKey(hwnd, kHotkeyToggleId, 0, VK_F1)) {
        std::wcerr << L"RegisterHotKey(F1) failed, error=" << GetLastError() << std::endl;
    }
    if (!RegisterHotKey(hwnd, kHotkeyExitId, 0, VK_F2)) {
        std::wcerr << L"RegisterHotKey(F2) failed, error=" << GetLastError() << std::endl;
    }
    if (!RegisterHotKey(hwnd, kHotkeyDebugId, 0, VK_F3)) {
        std::wcerr << L"RegisterHotKey(F3) failed, error=" << GetLastError() << std::endl;
    }
    if (!RegisterHotKey(hwnd, kHotkeyScreenId, 0, VK_F4)) {
        std::wcerr << L"RegisterHotKey(F4) failed, error=" << GetLastError() << std::endl;
    }
    SetTimer(hwnd, kTimerId, static_cast<UINT>(std::max(30, m_app.UiRefreshIntervalMs())), nullptr);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_TAB_CLASSES;
    InitCommonControlsEx(&icc);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);
    m_tabControl = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE,
        0, 0, clientRect.right, kTabStripHeight, hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTabControlId)), m_hInstance, nullptr);
    SendMessageW(m_tabControl, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);

    wchar_t tabStatus[] = L"Status";
    wchar_t tabA[] = L"Sell Runo";
    wchar_t tabB[] = L"Buy Fish Head";
    wchar_t tabC[] = L"Sell Shiro";
    TCITEMW tie{};
    tie.mask = TCIF_TEXT;
    tie.pszText = tabStatus;
    TabCtrl_InsertItem(m_tabControl, 0, &tie);
    tie.pszText = tabA;
    TabCtrl_InsertItem(m_tabControl, 1, &tie);
    tie.pszText = tabB;
    TabCtrl_InsertItem(m_tabControl, 2, &tie);
    tie.pszText = tabC;
    TabCtrl_InsertItem(m_tabControl, 3, &tie);

    RECT panelBounds{0, kContentTop, clientRect.right, clientRect.bottom};
    m_macroPanelA.Create(hwnd, m_hInstance, kMacroPanelAIdBase, panelBounds);
    m_macroPanelB.Create(hwnd, m_hInstance, kMacroPanelBIdBase, panelBounds);
    m_macroPanelC.Create(hwnd, m_hInstance, kMacroPanelCIdBase, panelBounds);

    TabCtrl_SetCurSel(m_tabControl, 0);
    m_activeTab = 0;
}

void StatusWindow::OnDestroy() {
    UnregisterHotKey(m_hwnd, kHotkeyToggleId);
    UnregisterHotKey(m_hwnd, kHotkeyExitId);
    UnregisterHotKey(m_hwnd, kHotkeyDebugId);
    UnregisterHotKey(m_hwnd, kHotkeyScreenId);
    KillTimer(m_hwnd, kTimerId);
    m_hwnd = nullptr; // avoid a stale-handle DestroyWindow call from our own destructor
}

void StatusWindow::OnHotkey(int id) {
    switch (id) {
        case kHotkeyToggleId:
            std::wcout << L"[hotkey] F1 pressed" << std::endl;
            m_app.ToggleEnabled();
            break;
        case kHotkeyExitId:
            std::wcout << L"[hotkey] F2 pressed" << std::endl;
            PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            break;
        case kHotkeyDebugId:
            std::wcout << L"[hotkey] F3 pressed" << std::endl;
            m_app.ToggleDebug();
            break;
        case kHotkeyScreenId:
            std::wcout << L"[hotkey] F4 pressed" << std::endl;
            m_app.ToggleScreen();
            break;
        default:
            break;
    }
}

void StatusWindow::OnTimer() {
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void StatusWindow::OnNotify(LPARAM lParam) {
    const NMHDR* hdr = reinterpret_cast<const NMHDR*>(lParam);
    if (hdr->hwndFrom == m_tabControl && hdr->code == TCN_SELCHANGE) {
        OnTabChanged();
    }
}

void StatusWindow::OnCommand(WPARAM wParam) {
    if (m_macroPanelA.HandleCommand(wParam)) return;
    if (m_macroPanelB.HandleCommand(wParam)) return;
    m_macroPanelC.HandleCommand(wParam);
}

void StatusWindow::OnTabChanged() {
    m_activeTab = TabCtrl_GetCurSel(m_tabControl);
    m_macroPanelA.SetVisible(m_activeTab == 1);
    m_macroPanelB.SetVisible(m_activeTab == 2);
    m_macroPanelC.SetVisible(m_activeTab == 3);
    InvalidateRect(m_hwnd, nullptr, TRUE);
}

void StatusWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);

    // Double-buffer to avoid flicker from the frequent timer-driven repaints.
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, clientRect.right, clientRect.bottom);
    HGDIOBJ oldBmp = SelectObject(memDc, memBmp);

    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 20));
    FillRect(memDc, &clientRect, bg);
    DeleteObject(bg);

    UiSnapshot snap = m_app.GetSnapshot();
    Render(memDc, clientRect, snap);

    BitBlt(hdc, 0, 0, clientRect.right, clientRect.bottom, memDc, 0, 0, SRCCOPY);

    SelectObject(memDc, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDc);

    EndPaint(hwnd, &ps);
}

void StatusWindow::Render(HDC hdc, const RECT& clientRect, const UiSnapshot& snap) {
    if (m_activeTab != 0) return; // the macro config tabs are real child controls, not this custom paint

    SetBkMode(hdc, TRANSPARENT);

    HFONT font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ oldFont = SelectObject(hdc, font);

    RenderStatusText(hdc, 16, kContentTop, snap);
    (void)clientRect;

    SelectObject(hdc, oldFont);
    DeleteObject(font);
}

void StatusWindow::RenderStatusText(HDC hdc, int x, int y, const UiSnapshot& snap) {
    const int lineHeight = 22;
    int line = 0;
    auto put = [&](const std::wstring& text, COLORREF color) {
        SetTextColor(hdc, color);
        TextOutW(hdc, x, y + line * lineHeight, text.c_str(), static_cast<int>(text.size()));
        ++line;
    };

    put(L"FISHING BOT", RGB(230, 230, 230));
    ++line;

    put(snap.enabled ? L"Bot: ON  (F1 to disable)" : L"Bot: OFF (F1 to enable)",
        snap.enabled ? RGB(80, 220, 100) : RGB(200, 90, 90));

    std::wostringstream stateSs;
    stateSs << L"State: " << ToString(snap.state) << L"   elapsed=" << std::fixed << std::setprecision(0)
            << snap.stateElapsedMs << L"ms";
    put(stateSs.str(), RGB(220, 220, 120));
    ++line;

    std::wostringstream roiSs;
    roiSs << L"ROI: screen(" << snap.roi.screenX << L"," << snap.roi.screenY << L")  "
          << snap.roi.width << L"x" << snap.roi.height << L"  flank=" << snap.roi.flankPixels
          << L"  [screen " << (snap.activeScreenIndex + 1) << L", F4 to switch]";
    put(roiSs.str(), RGB(190, 190, 190));
    ++line;

    put(L"-- Raw detection (drives the completion timer) --", RGB(150, 150, 210));
    put(FormatBand(L"Marker", snap.lastDetection.marker), RGB(230, 90, 90));
    put(FormatBand(L"Target", snap.lastDetection.target), RGB(90, 210, 120));
    ++line;

    put(L"-- Tracked/predicted (drives control only) --", RGB(150, 150, 210));
    put(FormatTracked(L"Marker", snap.trackedMarker), RGB(240, 170, 70));
    put(FormatTracked(L"Target", snap.trackedTarget), RGB(120, 220, 150));
    ++line;

    std::wostringstream inputSs;
    inputSs << L"Mouse: " << (snap.mouseHeld ? L"DOWN" : L"up") << L"    T: " << (snap.tHeld ? L"DOWN" : L"up");
    put(inputSs.str(), RGB(220, 220, 220));

    std::wostringstream fpsSs;
    fpsSs << L"Capture FPS: " << std::fixed << std::setprecision(1) << snap.captureFps
          << L"    Detection FPS: " << std::setprecision(1) << snap.detectionFps;
    put(fpsSs.str(), RGB(180, 180, 180));

    put(snap.debugMode ? L"Debug overlay: shown on the fishing bar (F3)" : L"Debug overlay: hidden (F3)",
        RGB(150, 150, 210));
    ++line;

    if (snap.macroRunning) {
        const wchar_t* names[] = { L"Sell Runo (H+6)", L"Buy Fish Head (G+6)", L"Sell Shiro (H+7)" };
        std::wostringstream macroSs;
        macroSs << names[std::clamp(snap.activeMacroId, 0, 2)]
                << L": running (step " << (snap.macroStepIndex + 1) << L"/" << snap.macroStepCount << L")";
        put(macroSs.str(), RGB(235, 170, 235));
    } else {
        put(L"Macro: idle (H+6 Sell Runo / G+6 Buy Fish Head / H+7 Sell Shiro)", RGB(140, 140, 150));
    }

    if (!snap.statusMessage.empty()) {
        put(L"Status: " + snap.statusMessage, RGB(230, 150, 60));
    }

    put(L"", RGB(0,0,0));
    put(L"F1 bot  F2 exit  F3 debug  F4 screen  H+6/G+6/H+7 macros", RGB(140, 140, 140));
}

} // namespace fb
