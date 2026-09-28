#pragma once

#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>

namespace fb {

// Wraps SendInput with explicit, idempotent state tracking so that we only
// ever emit a down/up transition when the desired logical state actually
// changes. Never sends a raw down/up unconditionally on every tick.
//
// Safety contract: EmergencyReleaseAll() is safe to call from any thread,
// any number of times, including from a crash-handling path, and always
// forces the mouse button and every currently-tracked held key to the UP
// state regardless of what this object currently believes the state is.
class InputManager {
public:
    InputManager() = default;

    // Sets the desired logical state of the left mouse button. Only emits
    // SendInput when the state actually changes.
    void SetMouseLeft(bool down);

    // Sets the desired logical state of an arbitrary key (by virtual-key
    // code). Only emits SendInput when that key's tracked state actually
    // changes. Used for both the fishing loop's 'T' and the macro
    // sequences' T/D/A.
    void SetKey(uint8_t virtualKey, bool down);

    // Presses then releases the left mouse button with the given pulse
    // duration (blocking sleep - only used at state-machine transition
    // edges, never in the main per-frame loop).
    void ClickMouseLeftPulse(int pulseMs);

    // Moves the cursor smoothly from wherever it currently is to
    // (screenX, screenY) over approximately moveDurationMs (several
    // intermediate SetCursorPos steps with ease-in-out timing, not a
    // single jump), then presses and releases the left mouse button there
    // (blocking sleep for the whole move+pulse, same usage constraints as
    // ClickMouseLeftPulse - only at sequencer step edges, e.g. the macro
    // sequences).
    void ClickMouseLeftAt(int screenX, int screenY, int moveDurationMs, int pulseMs);

    // The move itself, without the click - exposed in case a future
    // sequencer step needs to reposition without clicking.
    //
    // Moves via a sequence of RELATIVE SendInput mouse events
    // (MOUSEEVENTF_MOVE, no MOUSEEVENTF_ABSOLUTE), not SetCursorPos.
    // SetCursorPos only teleports the OS cursor's absolute position - it
    // never emits the relative-delta input event stream a real mouse
    // produces, so a game reading raw input/DirectInput mouse deltas
    // (rather than polling absolute cursor position, which many games with
    // their own camera/aim handling do) never observes it moving at all.
    // Relative SendInput deltas are what an actual physical mouse's HID
    // reports look like, so this is what makes movement visible to those
    // games.
    static void MoveMouseSmooth(int screenX, int screenY, int moveDurationMs);

    // Presses then releases a key with the given pulse duration (blocking
    // sleep - same usage constraints as ClickMouseLeftPulse).
    void TapKeyPulse(uint8_t virtualKey, int pulseMs);

    bool IsMouseLeftDown() const { return m_mouseDown.load(std::memory_order_relaxed); }
    bool IsKeyDown(uint8_t virtualKey) const;

    // Forces both inputs to the UP state unconditionally. Safe to call
    // repeatedly (e.g. on F2, on exception, on shutdown).
    void EmergencyReleaseAll();

private:
    static void SendMouseButton(bool down);
    static void SendKey(uint8_t virtualKey, bool down);

    // Relative (not absolute) mouse motion - see MoveMouseSmooth's comment
    // for why this, rather than SetCursorPos, is what makes movement
    // visible to a game reading raw/DirectInput mouse deltas.
    static void SendRelativeMouseMove(int dx, int dy);

    std::atomic<bool> m_mouseDown{false};

    mutable std::mutex m_heldKeysMutex;
    std::vector<uint8_t> m_heldKeys; // small (T/D/A at most) - linear scan is fine
};

} // namespace fb
