#include "InputManager.h"

#include <Windows.h>
#include <thread>
#include <chrono>
#include <algorithm>

namespace fb {

namespace {
constexpr uint8_t kVkT = 0x54; // 'T' - the fishing loop's own recast key, force-released defensively below
}

void InputManager::SendMouseButton(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputManager::SendKey(uint8_t virtualKey, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = virtualKey;
    // Many games read the hardware scan code (via DirectInput/raw input)
    // rather than relying on the standard WM_KEYDOWN message queue, and
    // silently ignore a SendInput event that carries only a virtual-key
    // code with no scan code. Including it costs nothing for apps that
    // only look at wVk, and fixes delivery for the ones that don't.
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputManager::SetMouseLeft(bool down) {
    // exchange() always stores `down`; only emit SendInput if the state
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

void InputManager::SendRelativeMouseMove(int dx, int dy) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    // No MOUSEEVENTF_ABSOLUTE: dx/dy are relative mickeys, matching a real
    // mouse's HID reports - see the declaration comment for why this
    // matters (SetCursorPos does not produce equivalent input for a game
    // reading raw/DirectInput deltas).
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(INPUT));
}

void InputManager::MoveMouseSmooth(int screenX, int screenY, int moveDurationMs) {
    POINT start{};
    if (!GetCursorPos(&start)) return;

    // A fixed ~8ms step interval gives a smooth-looking glide without
    // being finer-grained than useful; step COUNT then follows from the
    // requested duration, so a longer move takes more steps rather than
    // slower steps.
    constexpr int kStepIntervalMs = 8;
    int steps = std::max(1, moveDurationMs / kStepIntervalMs);

    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        float eased = t * t * (3.0f - 2.0f * t); // smoothstep: ease-in-out, not linear/robotic
        int targetX = start.x + static_cast<int>(static_cast<float>(screenX - start.x) * eased);
        int targetY = start.y + static_cast<int>(static_cast<float>(screenY - start.y) * eased);

        // Re-measure rather than accumulate an intended running position:
        // Windows' pointer-precision/acceleration curve can distort a
        // relative delta's actual effect, so each step corrects against
        // where the cursor really is, not where the last step assumed it
        // landed.
        POINT current{};
        if (GetCursorPos(&current)) {
            int dx = targetX - current.x;
            int dy = targetY - current.y;
            if (dx != 0 || dy != 0) SendRelativeMouseMove(dx, dy);
        }
        if (i < steps) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStepIntervalMs));
        }
    }

    // Final correction pass: the click coordinates are exact game-UI pixel
    // targets, not approximate, so iron out any residual drift from the
    // acceleration curve rather than accepting "close enough".
    for (int attempt = 0; attempt < 3; ++attempt) {
        POINT current{};
        if (!GetCursorPos(&current)) break;
        int dx = screenX - current.x;
        int dy = screenY - current.y;
        if (dx == 0 && dy == 0) break;
        SendRelativeMouseMove(dx, dy);
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
    // correctly (e.g. a later SetMouseLeft(false) won't be treated as a
    // no-op just because we think it's already false when SendInput was
    // never actually issued for this particular release).
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

} // namespace fb
