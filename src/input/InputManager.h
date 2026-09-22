#pragma once

#include <atomic>

namespace fb {

// Wraps SendInput with explicit, idempotent state tracking so that we only
// ever emit a down/up transition when the desired logical state actually
// changes. Never sends a raw down/up unconditionally on every tick.
//
// Safety contract: EmergencyReleaseAll() is safe to call from any thread,
// any number of times, including from a crash-handling path, and always
// forces both mouse-left and 'T' to the UP state regardless of what this
// object currently believes the state is.
class InputManager {
public:
    InputManager() = default;

    // Sets the desired logical state of the left mouse button. Only emits
    // SendInput when the state actually changes.
    void SetMouseLeft(bool down);

    // Sets the desired logical state of the 'T' key. Only emits SendInput
    // when the state actually changes.
    void SetKeyT(bool down);

    // Presses then releases the left mouse button with the given pulse
    // duration (blocking sleep - only used at state-machine transition
    // edges, never in the main per-frame loop).
    void ClickMouseLeftPulse(int pulseMs);

    bool IsMouseLeftDown() const { return m_mouseDown.load(std::memory_order_relaxed); }
    bool IsKeyTDown() const { return m_tDown.load(std::memory_order_relaxed); }

    // Forces both inputs to the UP state unconditionally. Safe to call
    // repeatedly (e.g. on F2, on exception, on shutdown).
    void EmergencyReleaseAll();

private:
    static void SendMouseButton(bool down);
    static void SendKeyT(bool down);

    std::atomic<bool> m_mouseDown{false};
    std::atomic<bool> m_tDown{false};
};

} // namespace fb
