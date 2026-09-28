#pragma once

#include "../common/Types.h"
#include "../config/Config.h"
#include "../capture/DesktopDuplication.h"
#include "../tracking/Tracker.h"
#include "../controller/FishingController.h"
#include "../input/InputManager.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>
#include <string>

namespace fb {

// Everything the UI needs to render, captured as one coherent, thread-safe
// snapshot. All coordinates inside DetectionResult/TrackedBand here are
// roiLocal - the UI is responsible for converting to debug-panel pixels
// via RoiLocalYToDebugY(), never by re-deriving screen coordinates.
struct UiSnapshot {
    bool enabled = false;
    BotState state = BotState::Off;
    float stateElapsedMs = 0.0f;

    RoiConfig roi;
    ControllerConfig controllerConfig; // copied in for debug-panel deadband rendering
    bool debugMode = true;

    DetectionResult lastDetection; // raw, roiLocal; masks populated iff debugMode was on
    TrackedBand trackedMarker;
    TrackedBand trackedTarget;

    bool mouseHeld = false;
    bool tHeld = false;

    double captureFps = 0.0;
    double detectionFps = 0.0;

    int activeScreenIndex = 0; // 0 = screen 1, 1 = screen 2 (F4 toggles)

    bool macroRunning = false; // H+6 (Sell Runo) / G+6 (Buy Fish Head) macro sequences
    int activeMacroId = 0;     // 0 = A "Sell Runo" (H+6), 1 = B "Buy Fish Head" (G+6); meaningful only while macroRunning
    int macroStepIndex = 0;
    int macroStepCount = 0;

    std::wstring statusMessage;
};

// Owns the capture thread, the detection/control thread, and the bot's
// state machine. The UI thread only ever touches this through GetSnapshot()
// (read) and ToggleEnabled()/RequestExit()/ToggleDebug() (write via atomics).
class App {
public:
    // Which of the two independent macro sequences (see App.cpp) is being
    // referred to. Public only so App.cpp's file-scope sequence table can
    // name it - nothing outside App constructs or inspects one; there is
    // no public trigger entry point (see CheckMacroHotkeys).
    enum class MacroId { A, B };

    explicit App(Config config);
    ~App();

    bool Start();
    void Stop();

    void ToggleEnabled();
    void RequestExit();
    void ToggleDebug();
    void ToggleScreen();

    bool ExitRequested() const { return m_exitRequested.load(std::memory_order_relaxed); }

    UiSnapshot GetSnapshot() const;

    int UiRefreshIntervalMs() const { return m_config.ui.refreshIntervalMs; }
    float OverlayRenderHz() const { return m_config.ui.overlayRenderHz; }
    bool IsDebugModeOn() const { return m_debugMode.load(std::memory_order_relaxed); }
    RoiConfig GetRoiConfig() const { return m_config.roi; } // the CONFIGURED (screen 1) ROI; immutable after construction

    // The ROI actually in effect right now (screen 1 or 2, per the F4
    // toggle) - this is what capture, and the debug overlay's on-screen
    // positioning, must use. Cheap (one atomic load + struct copy), safe
    // from any thread.
    RoiConfig CurrentRoiConfig() const;

private:
    void CaptureThreadMain();
    void DetectionThreadMain();
    void RunDetectionLoop();

    void TickStateMachine(const DetectionResult& raw, TimePoint now);
    void EnterState(BotState newState, TimePoint now);
    void UpdateSnapshot(const DetectionResult& raw, TimePoint now);
    void SetStatusMessage(const std::wstring& msg);

    // Two fixed, independent key/click macro sequences (A: "Sell Runo",
    // H+6 chord; B: "Buy Fish Head", G+6 chord), ticked instead of (never
    // alongside) the fishing state machine while either runs - see
    // App.cpp for the sequences themselves and why they're mutually
    // exclusive with fishing.
    //
    // Triggered by directly polling physical key state (GetAsyncKeyState)
    // every detection-loop tick rather than RegisterHotKey: RegisterHotKey
    // only supports one non-modifier key plus Alt/Ctrl/Shift/Win, not an
    // arbitrary chord of two plain keys like H+6. Polling happens on the
    // detection thread, the same thread that owns Start/Tick/Stop below,
    // so no cross-thread request signaling is needed at all.
    void CheckMacroHotkeys(TimePoint now);
    void OnMacroChordPressed(MacroId id, TimePoint now);
    void StartMacro(MacroId id, TimePoint now);
    void TickMacro(TimePoint now);
    void StopMacro(const std::string& reason);

    Config m_config;
    InputManager m_input;
    Tracker m_tracker;
    FishingController m_controller;
    DesktopDuplication m_duplication;

    std::thread m_captureThread;
    std::thread m_detectionThread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_exitRequested{false};
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_debugMode{true};
    std::atomic<int> m_activeScreenIndex{0}; // 0 or 1, see CurrentRoiConfig()

    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    CapturedFrame m_latestFrame;
    bool m_hasNewFrame = false;

    // Bot state machine - owned exclusively by the detection thread.
    BotState m_state = BotState::Off;
    TimePoint m_stateEnteredAt{};
    std::optional<TimePoint> m_bothAbsentSince;   // FISHING completion timer (raw)
    std::optional<TimePoint> m_bothPresentSince;  // CASTING -> FISHING trigger (raw)

    float m_captureFpsEma = 0.0f;
    float m_detectionFpsEma = 0.0f;

    // Macro sequence state - owned exclusively by the detection thread
    // (CheckMacroHotkeys/StartMacro/TickMacro/StopMacro all run there, via
    // RunDetectionLoop) - no atomics needed since nothing outside that
    // thread touches these.
    bool m_macroChordADown = false; // previous-tick state, for edge-detecting H+6 ("Sell Runo")
    bool m_macroChordBDown = false; // previous-tick state, for edge-detecting G+6 ("Buy Fish Head")
    bool m_macroRunning = false;
    MacroId m_activeMacroId = MacroId::A;
    size_t m_macroStepIndex = 0;
    enum class MacroPhase { Acting, InterDelay };
    MacroPhase m_macroPhase = MacroPhase::Acting;
    TimePoint m_macroPhaseStartedAt{};
    bool m_macroHoldKeyDown = false;

    mutable std::mutex m_snapshotMutex;
    UiSnapshot m_snapshot;
    std::mutex m_statusMutex;
    std::wstring m_statusMessage;
};

} // namespace fb
