#pragma once

#include "../common/Types.h"
#include "../config/Config.h"

namespace fb {

// Decides the desired left-mouse hold state during the FISHING state, from
// TRACKED/PREDICTED marker+target bands (never raw detections - the raw
// completion timer lives in the bot state machine, not here).
//
// Applies deadband + hysteresis (avoid chatter right at the boundary),
// a minimum toggle interval (avoid rapid on/off spam), and confidence
// gating (never make aggressive decisions from garbage data).
class FishingController {
public:
    explicit FishingController(const ControllerConfig& config) : m_config(config) {}

    // Call once per detection tick while in FISHING. Returns the desired
    // mouse-left-held state; caller forwards it to InputManager::SetMouseLeft
    // (which itself only emits SendInput on an actual state change).
    bool ComputeDesiredMouseHold(const TrackedBand& marker, const TrackedBand& target,
                                  TimePoint now, bool currentMouseHeld);

    void Reset();

private:
    const ControllerConfig& m_config;
    bool m_lastDesired = false;
    bool m_hasToggled = false;
    TimePoint m_lastToggleTime{};
    bool m_inLowConfidence = false;
    TimePoint m_lowConfidenceSince{};
};

} // namespace fb
