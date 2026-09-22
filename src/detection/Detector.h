#pragma once

#include "../common/Types.h"
#include "../config/Config.h"

namespace fb {

// An optional, externally-supplied hint about where the target was last
// known to be, used to make THIS FRAME's search spatially-informed. This
// does NOT make Detector stateful: the hint is just another input
// parameter, supplied fresh by the caller every call (typically derived
// from the Tracker's current prediction) - Detector itself still holds no
// memory between calls. See the target-detection block in Detector.cpp
// for why this exists: an absolute color match alone cannot reliably
// identify the target border against every background (a strongly blue
// background was found to shift the border's rendered color ~5x past the
// color-only tolerance), but the target can only move a little between
// two consecutive frames - so when a recent prior is available, requiring
// the candidate to be structurally right (correct run length/uniformity)
// AND near the prior is a strong substitute for an exact color match,
// with color kept only as a soft confidence booster instead of a hard
// gate. Without a usable prior (e.g. just re-acquiring after a genuine
// absence), the detector falls back to the strict, color-gated search.
struct TargetPriorHint {
    bool valid = false;
    float roiLocalCenterY = 0.0f;
    float maxDistancePx = 0.0f; // candidates farther than this from roiLocalCenterY are not considered
};

// Stateless, per-frame detector: holds no memory of its own between calls.
// This keeps raw visual detection (what the completion timer must use)
// strictly separate from tracked/predicted state (what the controller
// uses) - the optional TargetPriorHint parameter doesn't change that: it's
// an explicit input the caller must supply fresh each call, not something
// Detector remembers or accumulates itself.
class Detector {
public:
    explicit Detector(const Config& config) : m_config(config) {}

    // `frame` must be a capture covering roi.CaptureWidth() x roi.CaptureHeight()
    // pixels, with frame.captureOriginScreenX/Y set to the capture rect's
    // screen origin. `computeDebugMasks` controls whether the (relatively
    // expensive to copy to the UI) evidence masks are populated.
    // `targetPrior` is optional; pass a default-constructed (invalid) one
    // to force the strict, color-gated search unconditionally.
    DetectionResult Detect(const CapturedFrame& frame, bool computeDebugMasks,
                            const TargetPriorHint& targetPrior = {}) const;

private:
    const Config& m_config;
};

} // namespace fb
