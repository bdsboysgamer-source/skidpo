#pragma once

#include "../common/Types.h"
#include "../config/Config.h"

namespace fb {

// Stateless, per-frame detector. Never holds temporal state - that is the
// Tracker's job. This keeps raw visual detection (what the completion
// timer must use) strictly separate from tracked/predicted state (what the
// controller uses).
class Detector {
public:
    explicit Detector(const Config& config) : m_config(config) {}

    // `frame` must be a capture covering roi.CaptureWidth() x roi.CaptureHeight()
    // pixels, with frame.captureOriginScreenX/Y set to the capture rect's
    // screen origin. `computeDebugMasks` controls whether the (relatively
    // expensive to copy to the UI) evidence masks are populated.
    DetectionResult Detect(const CapturedFrame& frame, bool computeDebugMasks) const;

private:
    const Config& m_config;
};

} // namespace fb
