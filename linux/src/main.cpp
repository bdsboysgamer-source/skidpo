// Fishing bot entry point - Linux/Bazzite port.
//
// GUI NOT YET PORTED (see linux/README.md) - this is a direct, functionally
// complete port of everything except the status window / debug overlay's
// visuals. The Windows app already logs every state transition, hotkey
// press, and status change to a real console window alongside its GUI
// (see src/app/App.cpp's LogLine calls) - this build does the exact same
// logging, plus a periodic status line standing in for what the GUI would
// otherwise show, until the GUI itself is ported.
//
// F1-F4 and the macro chords (H+6/G+6/H+7) all work already - they're
// polled via physical key state, not tied to any window (see App.h).

#include "app/App.h"
#include "config/Config.h"

#include <X11/Xlib.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

using namespace fb;

namespace {
constexpr const char* kConfigPath = "config/config.ini";

std::atomic<bool>* g_exitFlagForSignalHandler = nullptr;

void HandleSigint(int) {
    if (g_exitFlagForSignalHandler) g_exitFlagForSignalHandler->store(true);
}

std::string FormatStatusLine(const UiSnapshot& snap) {
    std::ostringstream ss;
    ss << "[status] " << (snap.enabled ? "ON " : "OFF") << " state=" << ToString(snap.state)
       << " marker=" << (snap.lastDetection.marker.present ? "present" : "absent")
       << " target=" << (snap.lastDetection.target.present ? "present" : "absent")
       << " mouse=" << (snap.mouseHeld ? "DOWN" : "up") << " T=" << (snap.tHeld ? "DOWN" : "up")
       << " capFps=" << static_cast<int>(snap.captureFps) << " detFps=" << static_cast<int>(snap.detectionFps);
    if (snap.macroRunning) {
        const char* names[] = {"Sell Runo", "Buy Fish Head", "Sell Shiro"};
        int id = snap.activeMacroId >= 0 && snap.activeMacroId < 3 ? snap.activeMacroId : 0;
        ss << " macro=" << names[id] << " (" << (snap.macroStepIndex + 1) << "/" << snap.macroStepCount << ")";
    }
    return ss.str();
}

} // namespace

int main() {
    // X11 calls from multiple threads (capture thread, detection thread's
    // input polling) need this - without it, Xlib's behavior under
    // concurrent use from more than one thread is undefined.
    XInitThreads();

    std::cout << "=== Fishing Bot (Linux) ===" << std::endl;
    std::cout << "Loading configuration from " << kConfigPath << " (defaults used for missing/absent keys)" << std::endl;

    Config config = Config::LoadFromFile(kConfigPath);

    std::error_code ec;
    std::filesystem::create_directories("config", ec);
    config.SaveToFile(kConfigPath);

    std::cout << "ROI: screen(" << config.roi.screenX << "," << config.roi.screenY << ") "
              << config.roi.width << "x" << config.roi.height << " flank=" << config.roi.flankPixels << std::endl;

    App app(config, kConfigPath);

    std::atomic<bool> exitRequested{false};
    g_exitFlagForSignalHandler = &exitRequested;
    std::signal(SIGINT, HandleSigint);

    if (!app.Start()) {
        std::cerr << "Failed to start app threads." << std::endl;
        return 1;
    }

    std::cout << "Running. Press Ctrl+C to exit (or F2, from anywhere)." << std::endl;

    while (!app.ExitRequested() && !exitRequested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::cout << FormatStatusLine(app.GetSnapshot()) << std::endl;
    }

    app.Stop();
    std::cout << "Fishing Bot exited cleanly." << std::endl;
    return 0;
}
