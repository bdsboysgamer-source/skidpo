#include "InputManager.h"

#include <Windows.h>
#include <thread>
#include <chrono>

namespace fb {

void InputManager::SendMouseButton(bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &input, sizeof(INPUT));
}

void InputManager::SendKeyT(bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = 0x54; // 'T'
    // Many games read the hardware scan code (via DirectInput/raw input)
    // rather than relying on the standard WM_KEYDOWN message queue, and
    // silently ignore a SendInput event that carries only a virtual-key
    // code with no scan code. Including it costs nothing for apps that
    // only look at wVk, and fixes delivery for the ones that don't.
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(0x54, MAPVK_VK_TO_VSC));
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

void InputManager::SetKeyT(bool down) {
    if (m_tDown.exchange(down, std::memory_order_acq_rel) != down) {
        SendKeyT(down);
    }
}

void InputManager::ClickMouseLeftPulse(int pulseMs) {
    SetMouseLeft(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(pulseMs));
    SetMouseLeft(false);
}

void InputManager::EmergencyReleaseAll() {
    // Force UP unconditionally, independent of tracked state, then
    // reconcile the tracked state so subsequent SetX calls behave
    // correctly (e.g. a later SetMouseLeft(false) won't be treated as a
    // no-op just because we think it's already false when SendInput was
    // never actually issued for this particular release).
    SendMouseButton(false);
    SendKeyT(false);
    m_mouseDown.store(false, std::memory_order_release);
    m_tDown.store(false, std::memory_order_release);
}

} // namespace fb
