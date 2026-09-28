#include "App.h"
#include "../detection/Detector.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <cstdio>

namespace fb {

namespace {

std::string TimestampNow() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
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

std::string NarrowFromWide(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

constexpr uint8_t kVkT = 0x54; // the fishing loop's own recast key (WAIT_T state)

// ---- Macro sequences --------------------------------------------------
// The macro step definitions themselves (kind/vk/x/y, default timing) now
// live in Config (MacroConfig/MacroStepConfig - see config/Config.h/.cpp)
// since they're user-editable and persisted; App just ticks whichever
// macro is active. RepeatClick's fixed autoclick cadence, not user-
// configurable (only the burst's total duration is - see MacroStepConfig).
constexpr float kRepeatClickIntervalMs = 150.0f;

// ---- Macro hotkey chords -----------------------------------------------
// RegisterHotKey can't express an arbitrary chord of plain keys (only one
// non-modifier key plus Alt/Ctrl/Shift/Win), so these are detected by
// directly polling physical key state instead - see CheckMacroHotkeys.
constexpr uint8_t kChordVkH = 'H'; // H+6 -> macro A ("Sell Runo"); also H+7 -> macro C ("Sell Shiro")
constexpr uint8_t kChordVkG = 'G'; // G+6 -> macro B ("Buy Fish Head")
constexpr uint8_t kChordVk6 = '6';
constexpr uint8_t kChordVk7 = '7';

bool IsPhysicalKeyDown(uint8_t vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

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
        // Already stopped/stopping.
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

void App::SetStatusMessage(const std::wstring& msg) {
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        m_statusMessage = msg;
    }
    LogLine(NarrowFromWide(msg));
}

UiSnapshot App::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    return m_snapshot;
}

// -------------------------------------------------------------------
// Capture thread: DXGI Desktop Duplication -> newest-frame mailbox
// -------------------------------------------------------------------
void App::CaptureThreadMain() {
    RoiConfig activeRoi = CurrentRoiConfig();
    std::wstring err;
    if (!m_duplication.Initialize(activeRoi, err)) {
        SetStatusMessage(L"Capture init failed: " + err);
    } else {
        SetStatusMessage(L"Capture initialized (" + std::to_wstring(activeRoi.CaptureWidth()) + L"x"
                          + std::to_wstring(activeRoi.CaptureHeight()) + L" @ screen "
                          + std::to_wstring(activeRoi.CaptureOriginScreenX()) + L","
                          + std::to_wstring(activeRoi.CaptureOriginScreenY()) + L")");
    }

    TimePoint lastFrameTime = Clock::now();

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        // F4 (App::ToggleScreen) can move the ROI to a different monitor
        // at any time. DXGI Desktop Duplication is bound to one specific
        // output, so a screen change requires tearing down and
        // re-initializing against the new monitor - done here, on the
        // capture thread itself, since the duplication's D3D11/DXGI
        // objects aren't safe to touch from another thread concurrently
        // with this loop's own use of them.
        RoiConfig roiNow = CurrentRoiConfig();
        if (roiNow.screenX != activeRoi.screenX || roiNow.screenY != activeRoi.screenY) {
            LogLine("Capture ROI screen changed - reinitializing Desktop Duplication");
            m_duplication.Shutdown();
            activeRoi = roiNow;
        }

        if (!m_duplication.IsInitialized()) {
            std::wstring reinitErr;
            if (m_duplication.Initialize(activeRoi, reinitErr)) {
                SetStatusMessage(L"Capture (re)initialized");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        CapturedFrame frame;
        CaptureStatus status = m_duplication.AcquireFrame(activeRoi, frame, 100);

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
                SetStatusMessage(L"Capture device lost - reinitializing");
                m_duplication.Shutdown();
                break;
            case CaptureStatus::Failed:
                SetStatusMessage(L"Capture failed: " + m_duplication.LastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                break;
        }
    }

    m_duplication.Shutdown();
}

// -------------------------------------------------------------------
// Detection/control thread: newest frame -> detect -> track -> control
// -------------------------------------------------------------------
void App::DetectionThreadMain() {
    // Thin SEH wrapper with no local C++ objects of its own, so it can
    // safely catch structured exceptions (e.g. access violations) that a
    // plain try/catch(...) would not - guaranteeing the emergency input
    // release below runs even after a crash-class failure, not just a
    // thrown C++ exception.
    __try {
        RunDetectionLoop();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // No C++ objects (not even string temporaries) may live in this
        // function's scope alongside __try/__except (MSVC C2712), so log
        // with a plain C call rather than LogLine()/std::string.
        std::puts("[App] Detection thread: structured exception - releasing inputs");
        m_input.EmergencyReleaseAll();
    }
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
            // so the M+S+1 / M+S+B macro chords stay responsive even when
            // the screen is static and frames stop arriving; this loop
            // still wakes at least every ~50ms via the wait_for above.
            CheckMacroHotkeys(Clock::now());

            if (!gotFrame) continue;

            TimePoint now = Clock::now();
            float dt = std::chrono::duration<float>(now - lastTick).count();

            // Desktop Duplication can hand over new frames far faster than
            // is useful for a small border scan (400+fps observed, driven
            // by ANY compositor update anywhere on the display). Frames
            // arriving faster than the configured cap are dropped here,
            // not queued - low latency over processing every historical
            // frame, same philosophy as the capture thread's own
            // duplicate-frame skip.
            if (m_config.timing.detectionMaxHz > 0.0f) {
                float minIntervalSec = 1.0f / m_config.timing.detectionMaxHz;
                if (dt < minIntervalSec) continue;
            }
            lastTick = now;

            if (dt > 0.0001f) {
                m_detectionFpsEma = m_detectionFpsEma * 0.9f + (1.0f / dt) * 0.1f;
            }
            float clampedDt = std::max(dt, 1.0f / 240.0f);

            // Build the target search prior from the tracker's current
            // (pre-update) prediction, so this frame's detection can use
            // spatial continuity - the target can only move a little since
            // last frame - to stay locked on even when a strongly-colored
            // background pushes the color match alone out of tolerance.
            // See TargetPriorHint for why this doesn't make Detector
            // stateful: it's an explicit input, built fresh every call.
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

            // The macro sequences are fixed, independent key/click loops -
            // unrelated to fishing, and mutually exclusive with the
            // fishing state machine's own input control (both would
            // otherwise fight over the mouse/keyboard). Detection/
            // tracking above still runs normally either way; only the
            // control decision branches.
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
            // Detection-driven: move to FISHING as soon as both marker and
            // target have been continuously present (raw) for a short
            // debounce (filters a single noisy frame - not a meaningful
            // wait), rather than a fixed cast-animation duration.
            // castMaxWaitMs is a safety ceiling only, in case detection
            // never lands, so this state can't get stuck forever.
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

            // Completion timer uses RAW visual detection ONLY - tracked/
            // coasted state must never be able to defeat this.
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
            // Mouse is already released here; T isn't held yet - just a
            // plain pause before starting the T-hold sequence.
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

void App::CheckMacroHotkeys(TimePoint now) {
    bool aDown = IsPhysicalKeyDown(kChordVkH) && IsPhysicalKeyDown(kChordVk6);
    bool bDown = IsPhysicalKeyDown(kChordVkG) && IsPhysicalKeyDown(kChordVk6);
    bool cDown = IsPhysicalKeyDown(kChordVkH) && IsPhysicalKeyDown(kChordVk7);

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
    // Clean slate: don't let a mouse button the fishing controller happened
    // to be holding mid-reel interfere with the macro's own clicks.
    m_input.SetMouseLeft(false);
    m_macroStepIndex = 0;
    m_macroPhase = MacroPhase::Acting;
    m_macroPhaseStartedAt = now;
    m_macroHoldKeyDown = false;
    m_macroRunning = true;
}

void App::StopMacro(const std::string& reason) {
    // Release whatever key the sequence is currently mid-hold on before
    // stopping, so toggling a macro off can never leave e.g. T stuck down.
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
    // Copy the one step this tick needs (cheap - a handful of scalars) and
    // release the lock immediately, rather than holding it across the
    // input calls below (ClickMouseLeftAt/TapKeyPulse block for tens of ms
    // at a time), which would otherwise stall the UI thread's Save button.
    MacroStepConfig step{};
    size_t stepCount = 0;
    {
        std::lock_guard<std::mutex> lock(m_macroConfigMutex);
        const MacroConfig& mc = MacroConfigRefFor(m_activeMacroId);
        stepCount = mc.steps.size();
        if (stepCount == 0) return;
        if (m_macroStepIndex >= stepCount) m_macroStepIndex = 0; // defensive; wraps below before this could normally be hit
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
                    m_macroPhaseStartedAt = now; // start timing the hold itself
                } else if (ElapsedMs(m_macroPhaseStartedAt, now) >= step.durationSec * 1000.0f) {
                    m_input.SetKey(step.vk, false);
                    m_macroHoldKeyDown = false;
                    m_macroPhase = MacroPhase::InterDelay;
                    m_macroPhaseStartedAt = now; // now the inter-step delay starts
                }
                break;
            case MacroStepKind::RepeatClick: {
                // An autoclick burst: the cursor glides to the target once,
                // on the first click of the burst (detected by the last
                // click having happened before this step's Acting phase
                // began), then every subsequent click is a plain pulse in
                // place at a fixed cadence - re-gliding to the same spot
                // every 150ms would be pointless. Total burst length is
                // step.durationSec (user-editable); the 150ms cadence
                // itself is not.
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

    // InterDelay: the pause after this step, before the next one - now
    // per-step (step.delayAfterSec) rather than one global duration. The
    // sequence loops indefinitely (wraps back to step 0) rather than
    // stopping - pressing the active macro's chord again (OnMacroChordPressed,
    // via StopMacro) is the only way out.
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
    snap.roi = CurrentRoiConfig(); // reflects F4 screen toggle - the overlay positions itself off this
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
