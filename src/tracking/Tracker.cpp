#include "Tracker.h"

#include <cmath>
#include <algorithm>

namespace fb {

void Tracker::Reset() {
    m_marker = TrackedBand{};
    m_target = TrackedBand{};
    m_markerRejectStreak = 0;
    m_targetRejectStreak = 0;
    m_markerRejectedY = 0.0f;
    m_targetRejectedY = 0.0f;
    m_initialized = false;
}

void Tracker::Update(const DetectionResult& raw, TimePoint now, float dtSeconds) {
    if (!m_initialized) {
        m_markerLastRawSeen = now;
        m_targetLastRawSeen = now;
        m_initialized = true;
    }
    dtSeconds = std::max(dtSeconds, 1.0f / 240.0f);

    UpdateBand(m_marker, m_markerLastRawSeen, m_markerRejectStreak, m_markerRejectedY, raw.marker, now, dtSeconds);
    UpdateBand(m_target, m_targetLastRawSeen, m_targetRejectStreak, m_targetRejectedY, raw.target, now, dtSeconds);
}

void Tracker::UpdateBand(TrackedBand& band, TimePoint& lastRawSeen, int& rejectStreak, float& rejectedY,
                          const DetectionBand& raw, TimePoint now, float dtSeconds) {
    float predictedCenterY = band.hasData ? (band.centerY + band.velocityPerSec * dtSeconds) : 0.0f;

    if (raw.present) {
        float rawY = static_cast<float>(raw.roiLocalCenterY);

        if (!band.hasData) {
            band.hasData = true;
            band.centerY = rawY;
            band.velocityPerSec = 0.0f;
            band.top = static_cast<float>(raw.roiLocalTop);
            band.bottom = static_cast<float>(raw.roiLocalBottom);
            band.confidence = raw.confidence;
            band.consecutiveMisses = 0;
            band.coasting = false;
            rejectStreak = 0;
            lastRawSeen = now;
            return;
        }

        float maxJump = m_config.maxJumpPxPerSec * dtSeconds;
        float jump = std::fabs(rawY - predictedCenterY);
        bool accept = jump <= maxJump;

        if (!accept) {
            if (rejectStreak > 0 && std::fabs(rawY - rejectedY) <= std::max(maxJump, 1.0f)) {
                ++rejectStreak;
            } else {
                rejectStreak = 1;
                rejectedY = rawY;
            }
            if (rejectStreak >= m_config.reacquireFrames) {
                accept = true;
                rejectStreak = 0;
            }
        } else {
            rejectStreak = 0;
        }

        if (accept) {
            float residual = rawY - predictedCenterY;
            band.centerY = predictedCenterY + m_config.alpha * residual;
            band.velocityPerSec = band.velocityPerSec + (m_config.beta * residual) / dtSeconds;
            band.top += m_config.alpha * (static_cast<float>(raw.roiLocalTop) - band.top);
            band.bottom += m_config.alpha * (static_cast<float>(raw.roiLocalBottom) - band.bottom);
            band.confidence = raw.confidence;
            band.consecutiveMisses = 0;
            band.coasting = false;
            lastRawSeen = now;
            return;
        }

        // Rejected as an implausible jump: coast the prediction and treat
        // this frame like a miss for staleness purposes.
        band.centerY = predictedCenterY;
        band.consecutiveMisses++;
        band.coasting = true;
        float missMs = std::chrono::duration<float, std::milli>(now - lastRawSeen).count();
        if (missMs > static_cast<float>(m_config.missTimeoutMs)) {
            band.hasData = false;
            band.velocityPerSec = 0.0f;
            band.confidence = 0.0f;
            band.coasting = false;
        } else {
            band.confidence = std::max(0.0f, band.confidence - m_config.coastConfidenceDecayPerSec * dtSeconds);
        }
        return;
    }

    // No raw detection this frame.
    rejectStreak = 0;
    if (!band.hasData) return;

    band.consecutiveMisses++;
    float missMs = std::chrono::duration<float, std::milli>(now - lastRawSeen).count();
    if (missMs > static_cast<float>(m_config.missTimeoutMs)) {
        band.hasData = false;
        band.velocityPerSec = 0.0f;
        band.confidence = 0.0f;
        band.coasting = false;
    } else {
        band.centerY = predictedCenterY;
        band.coasting = true;
        band.confidence = std::max(0.0f, band.confidence - m_config.coastConfidenceDecayPerSec * dtSeconds);
    }
}

} // namespace fb
