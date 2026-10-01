#pragma once

#include "common/Types.h"
#include "config/Config.h"
#include "../capture/X11Capture.h"
#include "../input/InputManager.h"
#include "tracking/Tracker.h"
#include "controller/FishingController.h"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace fb {

// Everything a UI needs to render, captured as one coherent, thread-safe
// snapshot - direct port of the Windows UiSnapshot. statusMessage is a
// plain UTF-8 std::string here instead of std::wstring (Linux/Qt convention
// - std::wstring is a Windows-specific wide-string idiom), otherwise
// identical.
struct UiSnapshot {
    bool enabled = false;
    BotState state = BotState::Off;
    float stateElapsedMs = 0.0f;

    RoiConfig roi;
    ControllerConfig controllerConfig;
    bool debugMode = true;

    DetectionResult lastDetection;
    TrackedBand trackedMarker;
    TrackedBand trackedTarget;

    bool mouseHeld = false;
    bool tHeld = false;

    double captureFps = 0.0;
    double detectionFps = 0.0;

    int activeScreenIndex = 0; // 0 = screen 1, 1 = screen 2 (F4 toggles)

    bool macroRunning = false; // H+6 (Sell Runo) / G+6 (Buy Fish Head) / H+7 (Sell Shiro) macro sequences
    int activeMacroId = 0;     // 0 = A "Sell Runo" (H+6), 1 = B "Buy Fish Head" (G+6), 2 = C "Sell Shiro" (H+7); meaningful only while macroRunning
    int macroStepIndex = 0;
    int macroStepCount = 0;

    std::string statusMessage;
};

// Owns the capture thread, the detection/control thread, and the bot's
// state machine - direct port of the Windows App. Hotkeys (F1-F4) and the
// macro chords (H+6/G+6/H+7) are both polled via physical key state
// (InputManager::IsPhysicalKeyDown) on the detection thread, unlike the
// Windows version which used RegisterHotKey for F1-F4 specifically and
// physical polling only for the chords (RegisterHotKey can express a
// single key with no modifiers, as F1-F4 are, just as well as polling
// can) - unifying both onto one mechanism needs no GUI message loop to
// exist for hotkeys to work at all, which matters since this build has no
// GUI yet (see linux/README.md). Same user-visible behavior either way.
class App {
public:
    enum class MacroId { A, B, C };

    App(Config config, std::string configPath);
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
    RoiConfig GetRoiConfig() const { return m_config.roi; }

    RoiConfig CurrentRoiConfig() const;

    MacroConfig GetMacroConfig(MacroId id) const;
    void ApplyMacroStepTiming(MacroId id, const std::vector<MacroStepConfig>& editedSteps);

private:
    void CaptureThreadMain();
    void DetectionThreadMain();
    void RunDetectionLoop();

    void TickStateMachine(const DetectionResult& raw, TimePoint now);
    void EnterState(BotState newState, TimePoint now);
    void UpdateSnapshot(const DetectionResult& raw, TimePoint now);
    void SetStatusMessage(const std::string& msg);

    // F1-F4 global hotkeys, polled the same way as the macro chords below
    // (see the class comment for why).
    void CheckGlobalHotkeys();

    void CheckMacroHotkeys(TimePoint now);
    void OnMacroChordPressed(MacroId id, TimePoint now);
    void StartMacro(MacroId id, TimePoint now);
    void TickMacro(TimePoint now);
    void StopMacro(const std::string& reason);

    MacroConfig& MacroConfigRefFor(MacroId id);
    const MacroConfig& MacroConfigRefFor(MacroId id) const;

    Config m_config;
    std::string m_configPath;
    mutable std::mutex m_macroConfigMutex; // guards ONLY m_config.macroA/B/C - see the Windows App.h for the full rationale
    InputManager m_input;
    Tracker m_tracker;
    FishingController m_controller;
    X11Capture m_capture;

    std::thread m_captureThread;
    std::thread m_detectionThread;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_exitRequested{false};
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_debugMode{true};
    std::atomic<int> m_activeScreenIndex{0};

    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    CapturedFrame m_latestFrame;
    bool m_hasNewFrame = false;

    BotState m_state = BotState::Off;
    TimePoint m_stateEnteredAt{};
    std::optional<TimePoint> m_bothAbsentSince;
    std::optional<TimePoint> m_bothPresentSince;

    float m_captureFpsEma = 0.0f;
    float m_detectionFpsEma = 0.0f;

    // F1-F4 edge-detection state, mirroring the chord edge-detection below.
    bool m_hotkeyF1Down = false;
    bool m_hotkeyF2Down = false;
    bool m_hotkeyF3Down = false;
    bool m_hotkeyF4Down = false;

    bool m_macroChordADown = false;
    bool m_macroChordBDown = false;
    bool m_macroChordCDown = false;
    bool m_macroRunning = false;
    MacroId m_activeMacroId = MacroId::A;
    size_t m_macroStepIndex = 0;
    enum class MacroPhase { Acting, InterDelay };
    MacroPhase m_macroPhase = MacroPhase::Acting;
    TimePoint m_macroPhaseStartedAt{};
    bool m_macroHoldKeyDown = false;
    TimePoint m_macroLastRepeatClickAt{};

    mutable std::mutex m_snapshotMutex;
    UiSnapshot m_snapshot;
    std::mutex m_statusMutex;
    std::string m_statusMessage;
};

} // namespace fb
