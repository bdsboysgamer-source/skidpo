#pragma once

#include <X11/Xlib.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace fb {

// Linux/X11 counterpart to the Windows InputManager - same public contract
// (idempotent held-state tracking so a down/up is only ever emitted on an
// actual change, the EmergencyReleaseAll safety net, smooth relative-delta
// mouse movement) ported from SendInput to the XTest extension.
//
// Safety contract: EmergencyReleaseAll() is safe to call from any thread,
// any number of times, and always forces the mouse button and every
// currently-tracked held key to the UP state regardless of what this
// object currently believes the state is.
class InputManager {
public:
    InputManager();
    ~InputManager();

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Sets the desired logical state of the left mouse button. Only emits
    // an XTest event when the state actually changes.
    void SetMouseLeft(bool down);

    // Sets the desired logical state of an arbitrary key, by the same
    // virtual-key byte convention Config's MacroStepConfig::vk already
    // uses (plain uppercase ASCII letters - 'T', 'D', 'A', 'W', 'S' are
    // the ones this app actually sends). X11 keysyms for A-Z/0-9 are
    // numerically identical to ASCII, so these bytes need no translation
    // table - see SendKey.
    void SetKey(uint8_t virtualKey, bool down);

    // Presses then releases the left mouse button with the given pulse
    // duration (blocking sleep - only used at state-machine transition
    // edges, never in the main per-frame loop).
    void ClickMouseLeftPulse(int pulseMs);

    // Moves the cursor smoothly from wherever it currently is to
    // (screenX, screenY) over approximately moveDurationMs, then presses
    // and releases the left mouse button there (blocking sleep for the
    // whole move+pulse - same usage constraints as ClickMouseLeftPulse).
    void ClickMouseLeftAt(int screenX, int screenY, int moveDurationMs, int pulseMs);

    // The move itself, without the click.
    //
    // Moves via a sequence of RELATIVE XTest motion events
    // (XTestFakeRelativeMotionEvent), not an absolute warp
    // (XWarpPointer/XTestFakeMotionEvent is the X11 equivalent of
    // SetCursorPos). An absolute warp never produces the relative-delta
    // input-device event stream a real mouse does, so a game reading raw
    // input deltas rather than polling absolute cursor position never
    // observes it moving at all - this is the exact same issue found on
    // the Windows side (see that InputManager's own MoveMouseSmooth), and
    // Sober runs the literal same Roblox client code via Wine, so the
    // same fix almost certainly applies here too.
    //
    // Not static (unlike the Windows version) - reading/sending relative
    // to the current pointer position needs this object's X11 display
    // connection.
    void MoveMouseSmooth(int screenX, int screenY, int moveDurationMs);

    // Presses then releases a key with the given pulse duration (blocking
    // sleep - same usage constraints as ClickMouseLeftPulse).
    void TapKeyPulse(uint8_t virtualKey, int pulseMs);

    bool IsMouseLeftDown() const { return m_mouseDown.load(std::memory_order_relaxed); }
    bool IsKeyDown(uint8_t virtualKey) const;

    // Forces both inputs to the UP state unconditionally. Safe to call
    // repeatedly (e.g. on exit, on exception).
    void EmergencyReleaseAll();

    // Physical (real hardware) key state - the X11 counterpart to
    // GetAsyncKeyState, used by App for the F1-F4/H+6/G+6/H+7 polling.
    // Takes an X11 KeySym directly (e.g. XK_F1, or
    // static_cast<KeySym>('T') for the ASCII-range letters/digits this
    // app's own vk byte convention already uses).
    bool IsPhysicalKeyDown(KeySym keysym) const;

private:
    void SendMouseButton(bool down);
    void SendKey(uint8_t virtualKey, bool down);
    void SendKeySym(KeySym keysym, bool down);

    Display* m_display = nullptr; // the one X11 connection used for both XTest input and key-state polling

    std::atomic<bool> m_mouseDown{false};

    mutable std::mutex m_heldKeysMutex;
    std::vector<uint8_t> m_heldKeys; // small (T/D/A/W/S at most) - linear scan is fine
};

} // namespace fb
