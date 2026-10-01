#include "InputManager.h"

#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace fb {

namespace {
constexpr uint8_t kVkT = 0x54; // 'T' - force-released defensively below, mirroring the Windows version
}

InputManager::InputManager() {
    m_display = XOpenDisplay(nullptr);
}

InputManager::~InputManager() {
    if (m_display) {
        XCloseDisplay(m_display);
    }
}

void InputManager::SendMouseButton(bool down) {
    if (!m_display) return;
    XTestFakeButtonEvent(m_display, Button1, down ? True : False, CurrentTime);
    XFlush(m_display);
}

void InputManager::SendKeySym(KeySym keysym, bool down) {
    if (!m_display) return;
    KeyCode keycode = XKeysymToKeycode(m_display, keysym);
    if (keycode == 0) return;
    XTestFakeKeyEvent(m_display, keycode, down ? True : False, CurrentTime);
    XFlush(m_display);
}

void InputManager::SendKey(uint8_t virtualKey, bool down) {
    SendKeySym(static_cast<KeySym>(virtualKey), down);
}

void InputManager::SetMouseLeft(bool down) {
    // exchange() always stores `down`; only emit an event if the state
    // actually changed (i.e. the previous value differed).
    if (m_mouseDown.exchange(down, std::memory_order_acq_rel) != down) {
        SendMouseButton(down);
    }
}

void InputManager::SetKey(uint8_t virtualKey, bool down) {
    std::lock_guard<std::mutex> lock(m_heldKeysMutex);
    auto it = std::find(m_heldKeys.begin(), m_heldKeys.end(), virtualKey);
    bool currentlyHeld = (it != m_heldKeys.end());
    if (currentlyHeld == down) return; // no change

    SendKey(virtualKey, down);
    if (down) {
        m_heldKeys.push_back(virtualKey);
    } else {
        m_heldKeys.erase(it);
    }
}

bool InputManager::IsKeyDown(uint8_t virtualKey) const {
    std::lock_guard<std::mutex> lock(m_heldKeysMutex);
    return std::find(m_heldKeys.begin(), m_heldKeys.end(), virtualKey) != m_heldKeys.end();
}

void InputManager::ClickMouseLeftPulse(int pulseMs) {
    SetMouseLeft(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(pulseMs));
    SetMouseLeft(false);
}

void InputManager::MoveMouseSmooth(int screenX, int screenY, int moveDurationMs) {
    if (!m_display) return;

    Window root = DefaultRootWindow(m_display);
    Window retRoot = 0, retChild = 0;
    int startX = 0, startY = 0, winX = 0, winY = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(m_display, root, &retRoot, &retChild, &startX, &startY, &winX, &winY, &mask)) {
        return;
    }

    // A fixed ~8ms step interval gives a smooth-looking glide without
    // being finer-grained than useful; step COUNT then follows from the
    // requested duration, so a longer move takes more steps rather than
    // slower steps. Mirrors the Windows version's MoveMouseSmooth exactly.
    constexpr int kStepIntervalMs = 8;
    int steps = std::max(1, moveDurationMs / kStepIntervalMs);

    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float eased = t * t * (3.0f - 2.0f * t); // smoothstep: ease-in-out, not linear/robotic
        int targetX = startX + static_cast<int>(static_cast<float>(screenX - startX) * eased);
        int targetY = startY + static_cast<int>(static_cast<float>(screenY - startY) * eased);

        // Re-measure rather than accumulate an intended running position,
        // same reasoning as the Windows version: whatever pointer-
        // acceleration curve is active can distort a relative delta's
        // actual effect, so each step corrects against where the cursor
        // really is, not where the last step assumed it landed.
        int curX = 0, curY = 0;
        if (XQueryPointer(m_display, root, &retRoot, &retChild, &curX, &curY, &winX, &winY, &mask)) {
            int dx = targetX - curX;
            int dy = targetY - curY;
            if (dx != 0 || dy != 0) {
                XTestFakeRelativeMotionEvent(m_display, dx, dy, CurrentTime);
                XFlush(m_display);
            }
        }
        if (i < steps) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStepIntervalMs));
        }
    }

    // Final correction pass: the click coordinates are exact game-UI pixel
    // targets, not approximate, so iron out any residual drift rather than
    // accepting "close enough".
    for (int attempt = 0; attempt < 3; ++attempt) {
        int curX = 0, curY = 0;
        if (!XQueryPointer(m_display, root, &retRoot, &retChild, &curX, &curY, &winX, &winY, &mask)) break;
        int dx = screenX - curX;
        int dy = screenY - curY;
        if (dx == 0 && dy == 0) break;
        XTestFakeRelativeMotionEvent(m_display, dx, dy, CurrentTime);
        XFlush(m_display);
    }
}

void InputManager::ClickMouseLeftAt(int screenX, int screenY, int moveDurationMs, int pulseMs) {
    MoveMouseSmooth(screenX, screenY, moveDurationMs);
    SetMouseLeft(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(pulseMs));
    SetMouseLeft(false);
}

void InputManager::TapKeyPulse(uint8_t virtualKey, int pulseMs) {
    SetKey(virtualKey, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(pulseMs));
    SetKey(virtualKey, false);
}

void InputManager::EmergencyReleaseAll() {
    // Force UP unconditionally, independent of tracked state, then
    // reconcile the tracked state so subsequent SetX calls behave
    // correctly - mirrors the Windows version's EmergencyReleaseAll.
    SendMouseButton(false);
    m_mouseDown.store(false, std::memory_order_release);

    std::vector<uint8_t> keysToRelease;
    {
        std::lock_guard<std::mutex> lock(m_heldKeysMutex);
        keysToRelease = std::move(m_heldKeys);
        m_heldKeys.clear();
    }
    for (uint8_t vk : keysToRelease) {
        SendKey(vk, false);
    }
    // Defense in depth: force T up even if our own bookkeeping somehow
    // missed it - it's the one key every fishing-loop cycle depends on.
    SendKey(kVkT, false);
}

bool InputManager::IsPhysicalKeyDown(KeySym keysym) const {
    if (!m_display) return false;
    KeyCode keycode = XKeysymToKeycode(m_display, keysym);
    if (keycode == 0) return false;

    char keymap[32];
    XQueryKeymap(m_display, keymap);
    return (keymap[keycode / 8] & (1 << (keycode % 8))) != 0;
}

} // namespace fb
