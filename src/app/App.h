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

    std::wstring statusMessage;
};

// Owns the capture thread, the detection/control thread, and the bot's
// state machine. The UI thread only ever touches this through GetSnapshot()
// (read) and ToggleEnabled()/RequestExit()/ToggleDebug() (write via atomics).
class App {
public:
    explicit App(Config config);
    ~App();

    bool Start();
    void Stop();

    void ToggleEnabled();
    void RequestExit();
    void ToggleDebug();

    bool ExitRequested() const { return m_exitRequested.load(std::memory_order_relaxed); }

    UiSnapshot GetSnapshot() const;

    int UiRefreshIntervalMs() const { return m_config.ui.refreshIntervalMs; }
    RoiConfig GetRoiConfig() const { return m_config.roi; } // immutable after construction; safe from any thread

private:
    void CaptureThreadMain();
    void DetectionThreadMain();
    void RunDetectionLoop();

    void TickStateMachine(const DetectionResult& raw, TimePoint now);
    void EnterState(BotState newState, TimePoint now);
    void UpdateSnapshot(const DetectionResult& raw, TimePoint now);
    void SetStatusMessage(const std::wstring& msg);

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

    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    CapturedFrame m_latestFrame;
    bool m_hasNewFrame = false;

    // Bot state machine - owned exclusively by the detection thread.
    BotState m_state = BotState::Off;
    TimePoint m_stateEnteredAt{};
    std::optional<TimePoint> m_bothAbsentSince;

    float m_captureFpsEma = 0.0f;
    float m_detectionFpsEma = 0.0f;

    mutable std::mutex m_snapshotMutex;
    UiSnapshot m_snapshot;
    std::mutex m_statusMutex;
    std::wstring m_statusMessage;
};

} // namespace fb
