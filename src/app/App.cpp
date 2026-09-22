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

} // namespace

App::App(Config config)
    : m_config(std::move(config))
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
    std::wstring err;
    if (!m_duplication.Initialize(m_config.roi, err)) {
        SetStatusMessage(L"Capture init failed: " + err);
    } else {
        SetStatusMessage(L"Capture initialized (" + std::to_wstring(m_config.roi.CaptureWidth()) + L"x"
                          + std::to_wstring(m_config.roi.CaptureHeight()) + L" @ screen "
                          + std::to_wstring(m_config.roi.CaptureOriginScreenX()) + L","
                          + std::to_wstring(m_config.roi.CaptureOriginScreenY()) + L")");
    }

    TimePoint lastFrameTime = Clock::now();

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        if (!m_duplication.IsInitialized()) {
            std::wstring reinitErr;
            if (m_duplication.Initialize(m_config.roi, reinitErr)) {
                SetStatusMessage(L"Capture (re)initialized");
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
        }

        CapturedFrame frame;
        CaptureStatus status = m_duplication.AcquireFrame(m_config.roi, frame, 100);

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
            if (!gotFrame) continue;

            TimePoint now = Clock::now();
            float dt = std::chrono::duration<float>(now - lastTick).count();
            lastTick = now;
            if (dt > 0.0001f) {
                m_detectionFpsEma = m_detectionFpsEma * 0.9f + (1.0f / dt) * 0.1f;
            }

            bool debugMode = m_debugMode.load(std::memory_order_relaxed);
            DetectionResult raw = detector.Detect(frame, debugMode);
            m_tracker.Update(raw, now, std::max(dt, 1.0f / 240.0f));

            TickStateMachine(raw, now);
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
            EnterState(BotState::Off, now);
        }
        return;
    }

    switch (m_state) {
        case BotState::Off: {
            m_input.ClickMouseLeftPulse(m_config.timing.clickPulseMs);
            EnterState(BotState::Casting, now);
            break;
        }
        case BotState::Casting: {
            if (ElapsedMs(m_stateEnteredAt, now) >= static_cast<float>(m_config.timing.castDelayMs)) {
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
                    m_input.SetKeyT(true);
                    m_controller.Reset();
                    EnterState(BotState::WaitT, now);
                }
            } else {
                m_bothAbsentSince.reset();
            }
            break;
        }
        case BotState::WaitT: {
            if (ElapsedMs(m_stateEnteredAt, now) >= static_cast<float>(m_config.timing.tHoldMs)) {
                m_input.SetKeyT(false);
                EnterState(BotState::RecastWait, now);
            }
            break;
        }
        case BotState::RecastWait: {
            if (ElapsedMs(m_stateEnteredAt, now) >= static_cast<float>(m_config.timing.recastWaitMs)) {
                m_input.ClickMouseLeftPulse(m_config.timing.clickPulseMs);
                EnterState(BotState::Casting, now);
            }
            break;
        }
    }
}

void App::UpdateSnapshot(const DetectionResult& raw, TimePoint now) {
    UiSnapshot snap;
    snap.enabled = m_enabled.load(std::memory_order_relaxed);
    snap.state = m_state;
    snap.stateElapsedMs = ElapsedMs(m_stateEnteredAt, now);
    snap.roi = m_config.roi;
    snap.controllerConfig = m_config.controller;
    snap.debugMode = m_debugMode.load(std::memory_order_relaxed);
    snap.lastDetection = raw;
    snap.trackedMarker = m_tracker.Marker();
    snap.trackedTarget = m_tracker.Target();
    snap.mouseHeld = m_input.IsMouseLeftDown();
    snap.tHeld = m_input.IsKeyTDown();
    snap.captureFps = m_captureFpsEma;
    snap.detectionFps = m_detectionFpsEma;
    {
        std::lock_guard<std::mutex> lock(m_statusMutex);
        snap.statusMessage = m_statusMessage;
    }

    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_snapshot = std::move(snap);
}

} // namespace fb
