#pragma once

#include "../common/Types.h"
#include <string>
#include <cstdint>

namespace fb {

// Color prototype (0xRRGGBB packed, stored unpacked for convenience).
struct ProtoColor {
    uint8_t r, g, b;
};

struct TimingConfig {
    // CASTING -> FISHING is now detection-driven: as soon as both raw
    // marker and target have been continuously present for
    // castingDetectDebounceMs (filters a single noisy frame, not a
    // meaningful wait), the bot moves to FISHING. castMaxWaitMs is a
    // safety ceiling only - it fires if the cast animation/detection
    // never lands, so CASTING can't get stuck forever; it is not the
    // normal-path trigger.
    int castingDetectDebounceMs = 150;
    int castMaxWaitMs = 7000;

    int noObjectTimeoutMs = 2000;     // both raw marker+target absent -> completion
    int tHoldMs = 3000;               // T held duration
    int clickPulseMs = 60;            // press->release duration for a "click"
};

struct DetectionConfig {
    float markerMinPixelScore = 0.5f;

    // Target detection is border-based, not fill-based. The target zone's
    // left border renders as a SOLID (non-alpha-blended) vertical run of
    // pixels sharing almost exactly the same color, unlike the semi-
    // transparent fill (which was found to be unreliable to classify - see
    // git history/README). That run's own color fades smoothly between two
    // known endpoints depending on whether the player marker is currently
    // inside or outside the target, so it doubles as ground truth for "is
    // the marker in the zone right now", independent of comparing marker
    // vs. target position. The GUI is static (only the minigame's internal
    // state changes it), so the border's column is a known fixed location,
    // not something that needs to be searched for across the bar's width.
    int targetBorderColumnRoiLocalX = 0;     // roiLocalX of the strip (screenX 1410 == roi.screenX)
    int targetBorderColumnSearchWidth = 3;   // also checks a couple of columns to the right, for redundancy against transient capture noise - NOT a search for the border's location, which is fixed

    int targetBorderRunHeightPx = 24;        // the border's straight (non-curved) run length
    int targetBorderRunHeightTolerancePx = 6;
    float targetBorderUniformityTolerance = 6.0f; // max RGB-space deviation within a run for it to count as "flat"

    ProtoColor targetBorderColorInZone = {0x51, 0xDE, 0x09};    // marker inside the target
    ProtoColor targetBorderColorOutOfZone = {0xDA, 0xC8, 0x09}; // marker outside the target
    // Used as a hard gate ONLY when no usable prior position is available
    // (see TargetPriorHint) - a strongly-colored background (e.g. deep
    // blue) was found to shift the border's rendered color ~5x past this
    // tolerance, which a fixed color axis alone can never absorb. With a
    // prior, color is used as a soft confidence booster instead.
    float targetBorderColorAxisTolerance = 30.0f; // max perpendicular RGB distance from the in-zone<->out-of-zone color axis
    float targetBorderColorAxisMargin = 0.3f;     // how far past each endpoint (as a fraction of the axis length) is still accepted

    // Minimum per-frame search radius around the prior position, even at
    // very high detection FPS/tiny dt (where maxJumpPxPerSec*dt alone
    // would be near zero) - absorbs sub-pixel jitter/quantization in the
    // rendered position so a genuinely-stationary target isn't rejected
    // for being technically a pixel or two from the predicted center.
    float targetPriorSearchMinRadiusPx = 24.0f;

    int markerMinHeightPx = 3;
    int markerMinColumns = 17;           // out of roi.width columns (~28% of the 59px bar, same ratio as before)
    float markerMinFillRatio = 0.35f;
    int markerGapBridgePx = 2;

    float minConfidenceToReport = 0.4f;  // below this, DetectionBand.present = false
};

struct TrackingConfig {
    float maxJumpPxPerSec = 900.0f;      // implausible-jump rejection threshold
    int reacquireFrames = 3;             // consecutive rejected-but-agreeing frames to re-acquire
    int missTimeoutMs = 700;             // how long to coast on misses before declaring stale
    float alpha = 0.55f;                 // position filter gain
    float beta = 0.35f;                  // velocity filter gain
    float coastConfidenceDecayPerSec = 1.6f;
};

struct ControllerConfig {
    bool holdMovesMarkerUp = true;       // holding M1 decreases roiLocalY (moves marker up)
    float deadbandPx = 10.0f;
    int minToggleIntervalMs = 90;
    float minConfidenceForControl = 0.4f;
    int controllerCoastMs = 350;         // brief coast on low-confidence before forced release
    float predictionHorizonSec = 0.08f;  // how far ahead to extrapolate marker position
};

struct UiConfig {
    bool debugModeDefault = true;
    int refreshIntervalMs = 150;
};

struct Config {
    RoiConfig roi;
    TimingConfig timing;
    DetectionConfig detection;
    TrackingConfig tracking;
    ControllerConfig controller;
    UiConfig ui;

    static Config Defaults();

    // Loads from an INI-like key=value file; missing file or missing keys
    // silently fall back to defaults for that key.
    static Config LoadFromFile(const std::string& path);
    bool SaveToFile(const std::string& path) const;
};

} // namespace fb
