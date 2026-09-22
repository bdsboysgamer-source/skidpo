#include "FishingController.h"

#include <chrono>

namespace fb {

void FishingController::Reset() {
    m_lastDesired = false;
    m_hasToggled = false;
    m_inLowConfidence = false;
}

bool FishingController::ComputeDesiredMouseHold(const TrackedBand& marker, const TrackedBand& target,
                                                 TimePoint now, bool currentMouseHeld) {
    bool dataUsable = marker.hasData && target.hasData
                    && marker.confidence >= m_config.minConfidenceForControl
                    && target.confidence >= m_config.minConfidenceForControl;

    if (!dataUsable) {
        if (!m_inLowConfidence) {
            m_inLowConfidence = true;
            m_lowConfidenceSince = now;
        }
        float lowMs = std::chrono::duration<float, std::milli>(now - m_lowConfidenceSince).count();
        if (lowMs > static_cast<float>(m_config.controllerCoastMs)) {
            // Past the brief grace period with nothing usable: fail safe.
            m_lastDesired = false;
            return false;
        }
        // Within the grace period: hold whatever we were already doing
        // rather than reacting to noise.
        return currentMouseHeld;
    }
    m_inLowConfidence = false;

    float predictedMarkerY = marker.centerY + marker.velocityPerSec * m_config.predictionHorizonSec;
    float targetCenterY = (target.top + target.bottom) * 0.5f;

    bool desired = m_lastDesired;
    if (m_config.holdMovesMarkerUp) {
        // Holding M1 raises the marker (decreases roiLocalY): hold when
        // below the target center (+deadband), release when above.
        if (predictedMarkerY > targetCenterY + m_config.deadbandPx) desired = true;
        else if (predictedMarkerY < targetCenterY - m_config.deadbandPx) desired = false;
    } else {
        if (predictedMarkerY < targetCenterY - m_config.deadbandPx) desired = true;
        else if (predictedMarkerY > targetCenterY + m_config.deadbandPx) desired = false;
    }

    if (desired != m_lastDesired && m_hasToggled) {
        float sinceToggleMs = std::chrono::duration<float, std::milli>(now - m_lastToggleTime).count();
        if (sinceToggleMs < static_cast<float>(m_config.minToggleIntervalMs)) {
            desired = m_lastDesired; // suppress rapid oscillation
        }
    }

    if (desired != m_lastDesired) {
        m_lastToggleTime = now;
        m_hasToggled = true;
    }
    m_lastDesired = desired;
    return desired;
}

} // namespace fb
