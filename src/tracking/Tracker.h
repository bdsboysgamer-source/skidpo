#pragma once

#include "../common/Types.h"
#include "../config/Config.h"

namespace fb {

// Lightweight alpha-beta tracker producing SMOOTHED/PREDICTED band state for
// control purposes only.
//
// IMPORTANT: this class's output must never be used to decide whether the
// marker/target are "really" present for the purposes of the fishing bot's
// 5-second no-object completion timer. That decision must use the raw
// DetectionResult.marker.present / .target.present fields directly, taken
// straight from the Detector for the current frame. The Tracker will
// happily keep reporting hasData=true / hold a stale confidence value
// while coasting through missed detections - that's the whole point of a
// tracker - which makes it the wrong source of truth for "is it really
// gone".
class Tracker {
public:
    explicit Tracker(const TrackingConfig& config) : m_config(config) {}

    // `now` must be a monotonic steady_clock time. `dtSeconds` is the time
    // since the previous Update call.
    void Update(const DetectionResult& raw, TimePoint now, float dtSeconds);

    const TrackedBand& Marker() const { return m_marker; }
    const TrackedBand& Target() const { return m_target; }

    void Reset();

private:
    void UpdateBand(TrackedBand& band, TimePoint& lastRawSeen, int& rejectStreak, float& rejectedY,
                     const DetectionBand& raw, TimePoint now, float dtSeconds);

    const TrackingConfig& m_config;
    TrackedBand m_marker;
    TrackedBand m_target;
    TimePoint m_markerLastRawSeen{};
    TimePoint m_targetLastRawSeen{};
    int m_markerRejectStreak = 0;
    int m_targetRejectStreak = 0;
    float m_markerRejectedY = 0.0f;
    float m_targetRejectedY = 0.0f;
    bool m_initialized = false;
};

} // namespace fb
