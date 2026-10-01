#include "App.h"
#include "detection/Detector.h"

#include <X11/keysym.h>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <algorithm>

namespace fb {

namespace {

std::string TimestampNow() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
    localtime_r(&t, &tmBuf); // POSIX order (time_t*, tm*) - opposite of Windows' localtime_s
    std::ostringstream ss;
    ss << std::put_time(&tmBuf, "%H:%M:%S");
    return ss.str();
}

void LogLine(const std::string& msg) {
    std::cout << "[" << TimestampNow() << "] " << msg << std::endl;
}

float ElapsedMs(TimePoint from, TimePoint to) {
    return std::chrono::duration<float, std::milli>(to - from).count();
}

constexpr uint8_t kVkT = 0x54; // 'T' - the fishing loop's own recast key (WAIT_T state)

// ---- Macro sequences ---------------------------------------------------
// The macro step definitions themselves (kind/vk/x/y, default timing) live
// in Config (MacroConfig/MacroStepConfig - shared, unmodified); App just
// ticks whichever macro is active. RepeatClick's fixed autoclick cadence,
// not user-configurable (only the burst's total duration is).
constexpr float kRepeatClickIntervalMs = 150.0f;

// ---- Macro hotkey chords -------------------------------------------------
// Polled via physical key state (InputManager::IsPhysicalKeyDown), same as
// the Windows version - RegisterHotKey has no Linux/X11 equivalent anyway,
// and this doubles here as F1-F4's mechanism too (see App.h).
constexpr uint8_t kChordVkH = 'H'; // H+6 -> macro A ("Sell Runo"); also H+7 -> macro C ("Sell Shiro")
constexpr uint8_t kChordVkG = 'G'; // G+6 -> macro B ("Buy Fish Head")
constexpr uint8_t kChordVk6 = '6';
constexpr uint8_t kChordVk7 = '7';

} // namespace

App::App(Config config, std::string configPath)
    : m_config(std::move(config))
    , m_configPath(std::move(configPath))
    , m_input()
    , m_tracker(m_config.tracking)
    , m_controller(m_config.controller)
{
    m_debugMode.store(m_config.ui.debugModeDefault);
}

App::~App() {
    Stop();
}

bool App::Start() {
    m_stopRequested.store(false);
    m_state = BotState::Off;
    m_stateEnteredAt = Clock::now();

    m_captureThread = std::thread([this] { CaptureThreadMain(); });
    m_detectionThread = std::thread([this] { DetectionThreadMain(); });
    LogLine("App started. F1=toggle bot, F2=exit, F3=toggle debug.");
    return true;
}

void App::Stop() {
    if (m_stopRequested.exchange(true)) {
        if (m_captureThread.joinable()) m_captureThread.join();
        if (m_detectionThread.joinable()) m_detectionThread.join();
        return;
    }
    m_frameCv.notify_all();
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_detectionThread.joinable()) m_detectionThread.join();

    // Belt-and-suspenders: guarantee no stuck input even if a thread
    // exited through an unexpected path.
    m_input.EmergencyReleaseAll();
    LogLine("App stopped. Inputs released.");
}

void App::ToggleEnabled() {
    bool newVal = !m_enabled.load(std::memory_order_relaxed);
    m_enabled.store(newVal, std::memory_order_relaxed);
    LogLine(newVal ? "F1: bot ENABLED" : "F1: bot DISABLED");
}

void App::RequestExit() {
    LogLine("F2: exit requested");
    m_exitRequested.store(true, std::memory_order_relaxed);
    m_enabled.store(false, std::memory_order_relaxed);
    m_input.EmergencyReleaseAll();
}

void App::ToggleDebug() {
    bool newVal = !m_debugMode.load(std::memory_order_relaxed);
    m_debugMode.store(newVal, std::memory_order_relaxed);
    LogLine(newVal ? "F3: debug mode ON" : "F3: debug mode OFF");
}

void App::ToggleScreen() {
    int newVal = (m_activeScreenIndex.load(std::memory_order_relaxed) == 0) ? 1 : 0;
    m_activeScreenIndex.store(newVal, std::memory_order_relaxed);
    LogLine(newVal == 0 ? "F4: switched to screen 1" : "F4: switched to screen 2 (+" +
            std::to_string(m_config.roi.secondaryScreenOffsetXPx) + "px)");
}

RoiConfig App::CurrentRoiConfig() const {
    RoiConfig roi = m_config.roi;
    if (m_activeScreenIndex.load(std::memory_order_relaxed) != 0) {
        roi.screenX += roi.secondaryScreenOffsetXPx;
    }
    return roi;
}

MacroConfig& App::MacroConfigRefFor(MacroId id) {
    switch (id) {
        case MacroId::A: return m_config.macroA;
        case MacroId::B: return m_config.macroB;
        case MacroId::C: return m_config.macroC;
    }
    return m_config.macroA;
}

const MacroConfig& App::MacroConfigRefFor(MacroId id) const {
    switch (id) {
        case MacroId::A: return m_config.macroA;
        case MacroId::B: return m_config.macroB;
        case MacroId::C: return m_config.macroC;
    }
    return m_config.macroA;
}

MacroConfig App::GetMacroConfig(MacroId id) const {
    std::lock_guard<std::mutex> lock(m_macroConfigMutex);
    return MacroConfigRefFor(id);
}

void App::ApplyMacroStepTiming(MacroId id, const std::vector<MacroStepConfig>& editedSteps) {
    std::string macroName;
    {
        std::lock_guard<std::mutex> lock(m_macroConfigMutex);
        MacroConfig& mc = MacroConfigRefFor(id);
        macroName = mc.name;
        if (editedSteps.size() != mc.steps.size()) {
            LogLine("Macro config: edited step count mismatch, ignoring");
            return;
        }
        for (size_t i = 0; i < mc.steps.size(); ++i) {
            mc.steps[i].durationSec = std::max(0.0f, editedSteps[i].durationSec);
            mc.steps[i].delayAfterSec = std::max(0.0f, editedSteps[i].delayAfterSec);
        }
    }
    if (m_config.SaveToFile(m_configPath)) {
        LogLine(macroName + ": settings saved");
    } else {
        LogLine("Macro config: failed to save " + m_configPath);
    }
}

void App::SetStatusMessage(const std::string& msg) {
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusMessage = msg;
    }
    LogLine(msg);
}

UiSnapshot App::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    return m_snapshot;
}

// -------------------------------------------------------------------
// Capture thread: X11 screen-region capture -> newest-frame mailbox
// -------------------------------------------------------------------
void App::CaptureThreadMain() {
    RoiConfig activeRoi = CurrentRoiConfig();
    std::string err;
    if (!m_capture.Initialize(activeRoi, err)) {
        SetStatusMessage("Capture init failed: " + err);
    } else {
        SetStatusMessage("Capture initialized (" + std::to_string(activeRoi.CaptureWidth()) + "x"
                          + std::to_string(activeRoi.CaptureHeight()) + " @ screen "
                          + std::to_string(activeRoi.CaptureOriginScreenX()) + ","
                          + std::to_string(activeRoi.CaptureOriginScreenY()) + ")");
    }

    TimePoint lastFrameTime = Clock::now();

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        // F4 (App::ToggleScreen) can move the ROI to a different screen
        // offset at any time - reinitialize against the new region here,
        // on the capture thread itself.
        RoiConfig roiNow = CurrentRoiConfig();
        if (roiNow.screenX != activeRoi.screenX || roiNow.screenY != activeRoi.screenY) {
            LogLine("Capture ROI screen changed - reinitializing capture");
            m_capture.Shutdown();
            activeRoi = roiNow;
        }

        if (!m_capture.IsInitialized()) {
            std::string reinitErr;
            if (m_capture.Initialize(activeRoi, reinitErr)) {
                SetStatusMessage("Capture (re)initialized");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        CapturedFrame frame;
        CaptureStatus status = m_capture.AcquireFrame(activeRoi, frame, 100);

        switch (status) {
            case CaptureStatus::Ok: {
                TimePoint now = Clock::now();
                float dt = std::chrono::duration<float>(now - lastFrameTime).count();
                lastFrameTime = now;
                if (dt > 0.0001f) {
                    m_captureFpsEma = m_captureFpsEma * 0.9f + (1.0f / dt) * 0.1f;
                }
                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    m_latestFrame = std::move(frame);
                    m_hasNewFrame = true;
                }
                m_frameCv.notify_one();
                break;
            }
            case CaptureStatus::NoNewFrame:
                break;
            case CaptureStatus::DeviceLost:
                SetStatusMessage("Capture device lost - reinitializing");
                m_capture.Shutdown();
                break;
            case CaptureStatus::Failed:
                SetStatusMessage("Capture failed: " + m_capture.LastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                break;
        }

        // Unlike DXGI Desktop Duplication (event-driven, blocks until the
        // desktop actually changes), X11Capture::AcquireFrame is
        // synchronous and returns immediately - without a pace-limiting
        // sleep here this loop would spin at full CPU. detectionMaxHz
        // downstream caps how much of this actually gets processed; this
        // just caps how often we bother capturing at all.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    m_capture.Shutdown();
}

// -------------------------------------------------------------------
// Detection/control thread: newest frame -> detect -> track -> control
// -------------------------------------------------------------------
void App::DetectionThreadMain() {
    // No SEH equivalent exists in standard C++/POSIX the way Windows has
    // __try/__except - a genuine structured-exception-class failure (e.g.
    // a segfault) isn't catchable here any more than plain try/catch could
    // catch it on Windows either. RunDetectionLoop's own try/catch below
    // already handles ordinary C++ exceptions and releases inputs
    // regardless of which path triggered the exit.
    RunDetectionLoop();
}

void App::RunDetectionLoop() {
    Detector detector(m_config);
    TimePoint lastTick = Clock::now();

    try {
        while (!m_stopRequested.load(std::memory_order_relaxed)) {
            CapturedFrame frame;
            bool gotFrame = false;
            {
                std::unique_lock<std::mutex> lock(m_frameMutex);
                m_frameCv.wait_for(lock, std::chrono::milliseconds(50),
                    [this] { return m_hasNewFrame || m_stopRequested.load(std::memory_order_relaxed); });
                if (m_hasNewFrame) {
                    frame = std::move(m_latestFrame);
                    m_hasNewFrame = false;
                    gotFrame = true;
                }
            }
            if (m_stopRequested.load(std::memory_order_relaxed)) break;

            // Polled here - not gated behind gotFrame/detectionMaxHz below -
            // so F1-F4 and the macro chords stay responsive even when the
            // screen is static and frames stop arriving; this loop still
            // wakes at least every ~50ms via the wait_for above.
            CheckGlobalHotkeys();
            CheckMacroHotkeys(Clock::now());

            if (!gotFrame) continue;

            TimePoint now = Clock::now();
            float dt = std::chrono::duration<float>(now - lastTick).count();

            if (m_config.timing.detectionMaxHz > 0.0f) {
                float minIntervalSec = 1.0f / m_config.timing.detectionMaxHz;
                if (dt < minIntervalSec) continue;
            }
            lastTick = now;

            if (dt > 0.0001f) {
                m_detectionFpsEma = m_detectionFpsEma * 0.9f + (1.0f / dt) * 0.1f;
            }
            float clampedDt = std::max(dt, 1.0f / 240.0f);

            TargetPriorHint targetPrior;
            const TrackedBand& priorTarget = m_tracker.Target();
            if (priorTarget.hasData) {
                targetPrior.valid = true;
                targetPrior.roiLocalCenterY = priorTarget.centerY;
                targetPrior.maxDistancePx = m_config.tracking.maxJumpPxPerSec * clampedDt;
            }

            bool debugMode = m_debugMode.load(std::memory_order_relaxed);
            DetectionResult raw = detector.Detect(frame, debugMode, targetPrior);
            m_tracker.Update(raw, now, clampedDt);

            if (m_macroRunning) {
                TickMacro(now);
            } else {
                TickStateMachine(raw, now);
            }
            UpdateSnapshot(raw, now);
        }
    } catch (const std::exception& e) {
        LogLine(std::string("Detection thread exception: ") + e.what() + " - releasing inputs");
        m_input.EmergencyReleaseAll();
    } catch (...) {
        LogLine("Detection thread unknown exception - releasing inputs");
        m_input.EmergencyReleaseAll();
    }

    // Always release on the way out, whatever the exit path was.
    m_input.EmergencyReleaseAll();
}

void App::EnterState(BotState newState, TimePoint now) {
    LogLine(std::string("State: ") + ToString(m_state) + " -> " + ToString(newState));
    m_state = newState;
    m_stateEnteredAt = now;
}

void App::TickStateMachine(const DetectionResult& raw, TimePoint now) {
    bool enabled = m_enabled.load(std::memory_order_relaxed);

    if (!enabled) {
        if (m_state != BotState::Off) {
            m_input.EmergencyReleaseAll();
            m_controller.Reset();
            m_tracker.Reset();
            m_bothAbsentSince.reset();
            m_bothPresentSince.reset();
            EnterState(BotState::Off, now);
        }
        return;
    }

    switch (m_state) {
        case BotState::Off: {
            m_input.ClickMouseLeftPulse(m_config.timing.clickPulseMs);
            m_bothPresentSince.reset();
            EnterState(BotState::Casting, now);
            break;
        }
        case BotState::Casting: {
            bool bothPresentRaw = raw.marker.present && raw.target.present;
            if (bothPresentRaw) {
                if (!m_bothPresentSince.has_value()) {
                    m_bothPresentSince = now;
                }
                if (ElapsedMs(*m_bothPresentSince, now) >= static_cast<float>(m_config.timing.castingDetectDebounceMs)) {
                    m_bothAbsentSince.reset();
                    EnterState(BotState::Fishing, now);
                    break;
                }
            } else {
                m_bothPresentSince.reset();
            }

            if (ElapsedMs(m_stateEnteredAt, now) >= static_cast<float>(m_config.timing.castMaxWaitMs)) {
                LogLine("CASTING: detection never landed within castMaxWaitMs - proceeding to FISHING anyway");
                m_bothAbsentSince.reset();
                EnterState(BotState::Fishing, now);
            }
            break;
        }
        case BotState::Fishing: {
            bool desiredHold = m_controller.ComputeDesiredMouseHold(
                m_tracker.Marker(), m_tracker.Target(), now, m_input.IsMouseLeftDown());
            m_input.SetMouseLeft(desiredHold);

            bool bothAbsentRaw = !raw.marker.present && !raw.target.present;
            if (bothAbsentRaw) {
                if (!m_bothAbsentSince.has_value()) {
                    m_bothAbsentSince = now;
                }
                if (ElapsedMs(*m_bothAbsentSince, now) >= static_cast<float>(m_config.timing.noObjectTimeoutMs)) {
                    m_input.SetMouseLeft(false);
                    m_controller.Reset();
                    EnterState(BotState::PostFishDelay, now);
                }
            } else {
                m_bothAbsentSince.reset();
            }
            break;
        }
        case BotState::PostFishDelay: {
            if (ElapsedMs(m_stateEnteredAt, now) >= static_cast<float>(m_config.timing.postFishDelayMs)) {
                m_input.SetKey(kVkT, true);
                EnterState(BotState::WaitT, now);
            }
            break;
        }
        case BotState::WaitT: {
            if (ElapsedMs(m_stateEnteredAt, now) >= static_cast<float>(m_config.timing.tHoldMs)) {
                m_input.SetKey(kVkT, false);
                m_input.ClickMouseLeftPulse(m_config.timing.clickPulseMs);
                m_bothPresentSince.reset();
                EnterState(BotState::Casting, now);
            }
            break;
        }
    }
}

void App::CheckGlobalHotkeys() {
    bool f1 = m_input.IsPhysicalKeyDown(XK_F1);
    bool f2 = m_input.IsPhysicalKeyDown(XK_F2);
    bool f3 = m_input.IsPhysicalKeyDown(XK_F3);
    bool f4 = m_input.IsPhysicalKeyDown(XK_F4);

    if (f1 && !m_hotkeyF1Down) { LogLine("[hotkey] F1 pressed"); ToggleEnabled(); }
    m_hotkeyF1Down = f1;

    if (f2 && !m_hotkeyF2Down) { LogLine("[hotkey] F2 pressed"); RequestExit(); }
    m_hotkeyF2Down = f2;

    if (f3 && !m_hotkeyF3Down) { LogLine("[hotkey] F3 pressed"); ToggleDebug(); }
    m_hotkeyF3Down = f3;

    if (f4 && !m_hotkeyF4Down) { LogLine("[hotkey] F4 pressed"); ToggleScreen(); }
    m_hotkeyF4Down = f4;
}

void App::CheckMacroHotkeys(TimePoint now) {
    bool aDown = m_input.IsPhysicalKeyDown(static_cast<KeySym>(kChordVkH)) && m_input.IsPhysicalKeyDown(static_cast<KeySym>(kChordVk6));
    bool bDown = m_input.IsPhysicalKeyDown(static_cast<KeySym>(kChordVkG)) && m_input.IsPhysicalKeyDown(static_cast<KeySym>(kChordVk6));
    bool cDown = m_input.IsPhysicalKeyDown(static_cast<KeySym>(kChordVkH)) && m_input.IsPhysicalKeyDown(static_cast<KeySym>(kChordVk7));

    if (aDown && !m_macroChordADown) OnMacroChordPressed(MacroId::A, now);
    m_macroChordADown = aDown;

    if (bDown && !m_macroChordBDown) OnMacroChordPressed(MacroId::B, now);
    m_macroChordBDown = bDown;

    if (cDown && !m_macroChordCDown) OnMacroChordPressed(MacroId::C, now);
    m_macroChordCDown = cDown;
}

void App::OnMacroChordPressed(MacroId id, TimePoint now) {
    const char* chord = (id == MacroId::A) ? "H+6" : (id == MacroId::B) ? "G+6" : "H+7";
    std::string macroName;
    {
        std::lock_guard<std::mutex> lock(m_macroConfigMutex);
        macroName = MacroConfigRefFor(id).name;
    }
    std::string label = std::string(chord) + " (" + macroName + ")";

    if (m_macroRunning) {
        if (m_activeMacroId == id) {
            StopMacro(label + ": macro stopped");
        } else {
            LogLine(label + ": ignored, a different macro is already running");
        }
        return;
    }
    StartMacro(id, now);
    LogLine(label + ": macro started");
}

void App::StartMacro(MacroId id, TimePoint now) {
    m_activeMacroId = id;
    m_input.SetMouseLeft(false);
    m_macroStepIndex = 0;
    m_macroPhase = MacroPhase::Acting;
    m_macroPhaseStartedAt = now;
    m_macroHoldKeyDown = false;
    m_macroRunning = true;
}

void App::StopMacro(const std::string& reason) {
    if (m_macroHoldKeyDown) {
        uint8_t vk = 0;
        {
            std::lock_guard<std::mutex> lock(m_macroConfigMutex);
            const MacroConfig& mc = MacroConfigRefFor(m_activeMacroId);
            if (m_macroStepIndex < mc.steps.size()) vk = mc.steps[m_macroStepIndex].vk;
        }
        if (vk != 0) m_input.SetKey(vk, false);
        m_macroHoldKeyDown = false;
    }
    m_macroRunning = false;
    LogLine(reason);
}

void App::TickMacro(TimePoint now) {
    MacroStepConfig step{};
    size_t stepCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_macroConfigMutex);
        const MacroConfig& mc = MacroConfigRefFor(m_activeMacroId);
        stepCount = mc.steps.size();
        if (stepCount == 0) return;
        if (m_macroStepIndex >= stepCount) m_macroStepIndex = 0;
        step = mc.steps[m_macroStepIndex];
    }

    if (m_macroPhase == MacroPhase::Acting) {
        switch (step.kind) {
            case MacroStepKind::Tap:
                m_input.TapKeyPulse(step.vk, m_config.timing.clickPulseMs);
                m_macroPhase = MacroPhase::InterDelay;
                m_macroPhaseStartedAt = now;
                break;
            case MacroStepKind::Click:
                m_input.ClickMouseLeftAt(step.clickX, step.clickY, m_config.timing.macroMoveDurationMs, m_config.timing.clickPulseMs);
                m_macroPhase = MacroPhase::InterDelay;
                m_macroPhaseStartedAt = now;
                break;
            case MacroStepKind::Hold:
                if (!m_macroHoldKeyDown) {
                    m_input.SetKey(step.vk, true);
                    m_macroHoldKeyDown = true;
                    m_macroPhaseStartedAt = now;
                } else if (ElapsedMs(m_macroPhaseStartedAt, now) >= step.durationSec * 1000.0f) {
                    m_input.SetKey(step.vk, false);
                    m_macroHoldKeyDown = false;
                    m_macroPhase = MacroPhase::InterDelay;
                    m_macroPhaseStartedAt = now;
                }
                break;
            case MacroStepKind::RepeatClick: {
                bool isFirstClickOfBurst = m_macroLastRepeatClickAt < m_macroPhaseStartedAt;
                if (isFirstClickOfBurst) {
                    m_input.ClickMouseLeftAt(step.clickX, step.clickY, m_config.timing.macroMoveDurationMs, m_config.timing.clickPulseMs);
                    m_macroLastRepeatClickAt = now;
                } else if (ElapsedMs(m_macroLastRepeatClickAt, now) >= kRepeatClickIntervalMs) {
                    m_input.ClickMouseLeftPulse(m_config.timing.clickPulseMs);
                    m_macroLastRepeatClickAt = now;
                }
                if (ElapsedMs(m_macroPhaseStartedAt, now) >= step.durationSec * 1000.0f) {
                    m_macroPhase = MacroPhase::InterDelay;
                    m_macroPhaseStartedAt = now;
                }
                break;
            }
        }
        return;
    }

    if (ElapsedMs(m_macroPhaseStartedAt, now) >= step.delayAfterSec * 1000.0f) {
        ++m_macroStepIndex;
        if (m_macroStepIndex >= stepCount) {
            m_macroStepIndex = 0;
        }
        m_macroPhase = MacroPhase::Acting;
        m_macroPhaseStartedAt = now;
    }
}

void App::UpdateSnapshot(const DetectionResult& raw, TimePoint now) {
    UiSnapshot snap;
    snap.enabled = m_enabled.load(std::memory_order_relaxed);
    snap.state = m_state;
    snap.stateElapsedMs = ElapsedMs(m_stateEnteredAt, now);
    snap.roi = CurrentRoiConfig();
    snap.controllerConfig = m_config.controller;
    snap.debugMode = m_debugMode.load(std::memory_order_relaxed);
    snap.lastDetection = raw;
    snap.trackedMarker = m_tracker.Marker();
    snap.trackedTarget = m_tracker.Target();
    snap.mouseHeld = m_input.IsMouseLeftDown();
    snap.tHeld = m_input.IsKeyDown(kVkT);
    snap.captureFps = m_captureFpsEma;
    snap.detectionFps = m_detectionFpsEma;
    snap.activeScreenIndex = m_activeScreenIndex.load(std::memory_order_relaxed);
    snap.macroRunning = m_macroRunning;
    snap.activeMacroId = static_cast<int>(m_activeMacroId);
    snap.macroStepIndex = static_cast<int>(m_macroStepIndex);
    {
        std::lock_guard<std::mutex> lock(m_macroConfigMutex);
        snap.macroStepCount = static_cast<int>(MacroConfigRefFor(m_activeMacroId).steps.size());
    }
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        snap.statusMessage = m_statusMessage;
    }

    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_snapshot = std::move(snap);
}

} // namespace fb
