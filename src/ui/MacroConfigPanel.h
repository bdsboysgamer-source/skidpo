#pragma once

#include "../app/App.h"

#include <Windows.h>
#include <vector>

namespace fb {

// One macro's editable timing settings, rendered as a fixed list of rows
// (one per step: a description, an editable "delay after" field, and -
// only for steps that have one - an editable "duration" field) plus a
// Save button. A set of ordinary child controls (not a dialog template),
// created once at startup from that macro's step count (which never
// changes at runtime - only durationSec/delayAfterSec do), shown/hidden
// as a unit by StatusWindow when its tab is selected/deselected.
class MacroConfigPanel {
public:
    MacroConfigPanel(App& app, App::MacroId id);

    // parent: the window these controls live in (StatusWindow's HWND).
    // idBase: first control ID this panel may use - the caller must give
    // each panel a non-overlapping base (only the Save button's ID is
    // ever actually distinguished; see HandleCommand).
    bool Create(HWND parent, HINSTANCE hInstance, int idBase, const RECT& bounds);

    void SetVisible(bool visible);

    // Called from StatusWindow's WM_COMMAND handling. Returns true if the
    // command was this panel's Save button (and was handled).
    bool HandleCommand(WPARAM wParam);

private:
    struct RowControls {
        HWND label = nullptr;
        HWND delayCaption = nullptr;
        HWND delayEdit = nullptr;
        HWND durationCaption = nullptr; // null if this step kind has no duration
        HWND durationEdit = nullptr;    // null if this step kind has no duration
    };

    static std::wstring DescribeStep(size_t index, const MacroStepConfig& step);
    void OnSaveClicked();

    App& m_app;
    App::MacroId m_id;
    HWND m_parent = nullptr;
    HWND m_titleLabel = nullptr;
    std::vector<RowControls> m_rows;
    HWND m_saveButton = nullptr;
    HWND m_statusLabel = nullptr; // transient "Saved." / error feedback
    int m_saveButtonId = 0;
};

} // namespace fb
