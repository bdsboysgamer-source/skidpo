#include "MacroConfigPanel.h"

#include <sstream>
#include <string>

namespace fb {

namespace {

void ApplyDefaultFont(HWND hwnd) {
    if (hwnd) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
}

std::wstring WidenAscii(const std::string& s) {
    return std::wstring(s.begin(), s.end()); // macro names are ASCII-only; no real conversion needed
}

constexpr int kRowHeight = 26;
constexpr int kRowStartYOffset = 34;
constexpr int kLabelX = 10, kLabelW = 260;
constexpr int kDelayCapX = 280, kDelayCapW = 66;
constexpr int kDelayEditX = 350, kDelayEditW = 55;
constexpr int kDurCapX = 413, kDurCapW = 55;
constexpr int kDurEditX = 470, kDurEditW = 55;

} // namespace

MacroConfigPanel::MacroConfigPanel(App& app, App::MacroId id) : m_app(app), m_id(id) {}

std::wstring MacroConfigPanel::DescribeStep(size_t index, const MacroStepConfig& step) {
    std::wostringstream ss;
    ss << (index + 1) << L". ";
    switch (step.kind) {
        case MacroStepKind::Tap:
            ss << L"Tap " << static_cast<wchar_t>(step.vk);
            break;
        case MacroStepKind::Hold:
            ss << L"Hold " << static_cast<wchar_t>(step.vk);
            break;
        case MacroStepKind::Click:
            ss << L"Click (" << step.clickX << L"," << step.clickY << L")";
            break;
        case MacroStepKind::RepeatClick:
            ss << L"Repeat-click (" << step.clickX << L"," << step.clickY << L")";
            break;
    }
    return ss.str();
}

bool MacroConfigPanel::Create(HWND parent, HINSTANCE hInstance, int idBase, const RECT& bounds) {
    m_parent = parent;
    m_saveButtonId = idBase;

    MacroConfig mc = m_app.GetMacroConfig(m_id);

    int x0 = bounds.left;
    int y0 = bounds.top;
    int ctrlId = idBase + 1; // idBase itself is reserved for the Save button

    std::wstring title = WidenAscii(mc.name) + L" - delay/duration per step (seconds)";
    m_titleLabel = CreateWindowExW(0, L"STATIC", title.c_str(), WS_CHILD | WS_VISIBLE,
        x0 + kLabelX, y0 + 8, 500, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId++)), hInstance, nullptr);
    ApplyDefaultFont(m_titleLabel);

    int rowStartY = y0 + kRowStartYOffset;
    m_rows.resize(mc.steps.size());
    wchar_t buf[32];

    for (size_t i = 0; i < mc.steps.size(); ++i) {
        int y = rowStartY + static_cast<int>(i) * kRowHeight;
        const MacroStepConfig& step = mc.steps[i];
        RowControls& row = m_rows[i];

        row.label = CreateWindowExW(0, L"STATIC", DescribeStep(i, step).c_str(), WS_CHILD | WS_VISIBLE,
            x0 + kLabelX, y, kLabelW, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId++)), hInstance, nullptr);
        ApplyDefaultFont(row.label);

        row.delayCaption = CreateWindowExW(0, L"STATIC", L"Delay (s):", WS_CHILD | WS_VISIBLE | SS_RIGHT,
            x0 + kDelayCapX, y + 2, kDelayCapW, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId++)), hInstance, nullptr);
        ApplyDefaultFont(row.delayCaption);

        swprintf(buf, 32, L"%.2f", static_cast<double>(step.delayAfterSec));
        row.delayEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            x0 + kDelayEditX, y, kDelayEditW, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId++)), hInstance, nullptr);
        ApplyDefaultFont(row.delayEdit);

        if (step.kind == MacroStepKind::Hold || step.kind == MacroStepKind::RepeatClick) {
            row.durationCaption = CreateWindowExW(0, L"STATIC", L"Dur (s):", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                x0 + kDurCapX, y + 2, kDurCapW, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId++)), hInstance, nullptr);
            ApplyDefaultFont(row.durationCaption);

            swprintf(buf, 32, L"%.2f", static_cast<double>(step.durationSec));
            row.durationEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", buf, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                x0 + kDurEditX, y, kDurEditW, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId++)), hInstance, nullptr);
            ApplyDefaultFont(row.durationEdit);
        }
    }

    int saveY = rowStartY + static_cast<int>(mc.steps.size()) * kRowHeight + 14;
    m_saveButton = CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        x0 + kLabelX, saveY, 100, 28, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(m_saveButtonId)), hInstance, nullptr);
    ApplyDefaultFont(m_saveButton);

    m_statusLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
        x0 + kLabelX + 112, saveY + 7, 320, 20, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ctrlId++)), hInstance, nullptr);
    ApplyDefaultFont(m_statusLabel);

    SetVisible(false); // hidden until its tab is selected
    return true;
}

void MacroConfigPanel::SetVisible(bool visible) {
    int cmd = visible ? SW_SHOW : SW_HIDE;
    ShowWindow(m_titleLabel, cmd);
    for (auto& row : m_rows) {
        ShowWindow(row.label, cmd);
        ShowWindow(row.delayCaption, cmd);
        ShowWindow(row.delayEdit, cmd);
        if (row.durationCaption) ShowWindow(row.durationCaption, cmd);
        if (row.durationEdit) ShowWindow(row.durationEdit, cmd);
    }
    ShowWindow(m_saveButton, cmd);
    ShowWindow(m_statusLabel, cmd);
}

bool MacroConfigPanel::HandleCommand(WPARAM wParam) {
    if (static_cast<int>(LOWORD(wParam)) != m_saveButtonId) return false;
    if (HIWORD(wParam) == BN_CLICKED) OnSaveClicked();
    return true;
}

void MacroConfigPanel::OnSaveClicked() {
    MacroConfig mc = m_app.GetMacroConfig(m_id);
    std::vector<MacroStepConfig> edited = mc.steps;
    wchar_t buf[64];
    bool ok = true;

    for (size_t i = 0; i < m_rows.size(); ++i) {
        GetWindowTextW(m_rows[i].delayEdit, buf, 64);
        try {
            edited[i].delayAfterSec = std::stof(std::wstring(buf));
        } catch (...) {
            ok = false;
        }

        if (m_rows[i].durationEdit) {
            GetWindowTextW(m_rows[i].durationEdit, buf, 64);
            try {
                edited[i].durationSec = std::stof(std::wstring(buf));
            } catch (...) {
                ok = false;
            }
        }
    }

    if (!ok) {
        SetWindowTextW(m_statusLabel, L"Invalid number - not saved");
        return;
    }
    m_app.ApplyMacroStepTiming(m_id, edited);
    SetWindowTextW(m_statusLabel, L"Saved.");
}

} // namespace fb
