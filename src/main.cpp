// Fishing bot entry point.
//
// Built as a CONSOLE subsystem application (see CMakeLists.txt) so that
// stdout/stderr logging (hotkey events, state transitions, capture errors)
// is visible without any AllocConsole/freopen plumbing, while still being
// completely free to create and drive ordinary Win32 windows and run a
// standard message loop from main().

#include "app/App.h"
#include "ui/StatusWindow.h"
#include "ui/DebugOverlay.h"
#include "config/Config.h"

#include <Windows.h>

#include <iostream>
#include <filesystem>

// Opts the whole process into Common Controls v6 (modern visual styles and,
// notably, full UI Automation provider support for standard controls like
// the tab control in StatusWindow) without needing a separate .manifest
// resource file - the linker embeds this as the executable's manifest.
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'\"")

using namespace fb;

namespace {
constexpr const char* kConfigPath = "config/config.ini";
}

int main() {
    std::cout << "=== Fishing Bot ===" << std::endl;
    std::cout << "Loading configuration from " << kConfigPath << " (defaults used for missing/absent keys)" << std::endl;

    Config config = Config::LoadFromFile(kConfigPath);

    // Make sure a config file exists on disk so the user has something to
    // tune; harmless if the directory doesn't exist yet or is read-only.
    std::error_code ec;
    std::filesystem::create_directories("config", ec);
    config.SaveToFile(kConfigPath);

    std::cout << "ROI: screen(" << config.roi.screenX << "," << config.roi.screenY << ") "
              << config.roi.width << "x" << config.roi.height << " flank=" << config.roi.flankPixels << std::endl;

    App app(config, kConfigPath);

    HINSTANCE hInstance = GetModuleHandleW(nullptr);
    StatusWindow window(app, hInstance);
    if (!window.Create(SW_SHOWNORMAL)) {
        std::cerr << "Failed to create status window." << std::endl;
        return 1;
    }

    // Overlay positioned over the game, next to (and, when the platform
    // supports SetWindowDisplayAffinity, directly on top of) the real
    // fishing bar. See ui/DebugOverlay.h for why that's capture-safe.
    DebugOverlay overlay(app, hInstance);
    if (!overlay.Create()) {
        std::cerr << "Failed to create debug overlay (continuing without it)." << std::endl;
    }

    if (!app.Start()) {
        std::cerr << "Failed to start app threads." << std::endl;
        return 1;
    }

    MSG msg;
    BOOL got;
    while ((got = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (got == -1) break; // GetMessage failure
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (app.ExitRequested() && !IsWindow(window.Handle())) break;
    }

    app.Stop();
    std::cout << "Fishing Bot exited cleanly." << std::endl;
    return 0;
}
