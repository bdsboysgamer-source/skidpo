#pragma once
//
// Types.h
//
// Shared POD types for the fishing bot. This header is the single source of
// truth for coordinate-space naming and conversion. See the comment block
// below before touching anything that moves a coordinate between spaces.
//
// -----------------------------------------------------------------------
// COORDINATE SPACES (do not mix these up)
// -----------------------------------------------------------------------
// screenX / screenY
//     Absolute virtual-desktop pixel coordinates, as used by SendInput,
//     DXGI output geometry, and RegisterHotKey/window placement.
//
// captureLocalX / captureLocalY
//     Pixel coordinates inside the CPU-readable capture buffer produced by
//     the desktop-duplication capture. (0,0) is the top-left of the
//     *captured rect*, which is the fishing ROI PLUS the left/right
//     background flank used for local background estimation. Width of the
//     capture buffer is therefore roi.width + 2*roi.flankPixels.
//
// roiLocalX / roiLocalY
//     Pixel coordinates inside the fishing bar ROI itself, i.e. NOT
//     including the flank. (0,0) is the top-left of the bar.
//     roiLocalX = captureLocalX - roi.flankPixels
//     roiLocalY = captureLocalY   (the ROI and the capture rect share the
//                                  same vertical extent; only X has flank)
//
// debugX / debugY
//     Pixel coordinates inside the debug schematic panel drawn in the
//     status window. This is a DIFFERENT SCALE from roiLocal - the bar is
//     384px tall in screen space but the debug schematic may be drawn at
//     a different pixel height. Always go through RoiLocalYToDebugY().
//
// Conversion rules:
//     screenY   = roi.screenY + roiLocalY            (add ROI_TOP once)
//     roiLocalY = screenY - roi.screenY               (subtract once)
//     debugY    = layout.debugTop +
//                 (roiLocalY / roi.height) * layout.debugHeight
//
// NEVER add roi.screenY to a value that already is a screen coordinate.
// NEVER treat a roiLocalY as if it were already a debugY or screenY.
// -----------------------------------------------------------------------

#include <cstdint>
#include <chrono>
#include <vector>
#include <optional>

namespace fb {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// ---------------------------------------------------------------------
// ROI configuration (screen space + flank)
// ---------------------------------------------------------------------
struct RoiConfig {
    // Measured directly against the live game via pixel inspection (the
    // target border's left strip runs from screen (1410,315) to
    // (1410,705); bar right edge measured separately at x=1469).
    int screenX = 1410;      // absolute screen X of the ROI (bar) top-left
    int screenY = 315;       // absolute screen Y of the ROI (bar) top-left
    int width = 59;          // bar width in pixels
    int height = 390;        // bar height in pixels
    int flankPixels = 16;    // background sample flank on each side

    int CaptureWidth() const { return width + 2 * flankPixels; }
    int CaptureHeight() const { return height; }

    // Screen X/Y of captureLocal (0,0)
    int CaptureOriginScreenX() const { return screenX - flankPixels; }
    int CaptureOriginScreenY() const { return screenY; }
};

inline int RoiLocalYToScreenY(const RoiConfig& roi, int roiLocalY) {
    return roi.screenY + roiLocalY;
}
inline int ScreenYToRoiLocalY(const RoiConfig& roi, int screenY) {
    return screenY - roi.screenY;
}
inline int RoiLocalXToScreenX(const RoiConfig& roi, int roiLocalX) {
    return roi.screenX + roiLocalX;
}
inline int CaptureLocalXToRoiLocalX(const RoiConfig& roi, int captureLocalX) {
    return captureLocalX - roi.flankPixels;
}
inline int RoiLocalXToCaptureLocalX(const RoiConfig& roi, int roiLocalX) {
    return roiLocalX + roi.flankPixels;
}

// Describes where the bar schematic is drawn inside the debug panel.
struct DebugBarLayout {
    int debugLeft = 0;
    int debugTop = 0;
    int debugWidth = 0;
    int debugHeight = 0;
};

inline int RoiLocalYToDebugY(const RoiConfig& roi, const DebugBarLayout& layout, int roiLocalY) {
    if (roi.height <= 0) return layout.debugTop;
    double t = static_cast<double>(roiLocalY) / static_cast<double>(roi.height);
    return layout.debugTop + static_cast<int>(t * layout.debugHeight);
}
inline int RoiLocalXToDebugX(const RoiConfig& roi, const DebugBarLayout& layout, int roiLocalX) {
    if (roi.width <= 0) return layout.debugLeft;
    double t = static_cast<double>(roiLocalX) / static_cast<double>(roi.width);
    return layout.debugLeft + static_cast<int>(t * layout.debugWidth);
}

// ---------------------------------------------------------------------
// Captured frame (CPU-readable BGRA buffer covering the capture rect)
// ---------------------------------------------------------------------
struct CapturedFrame {
    uint64_t frameId = 0;
    TimePoint timestamp{};
    int width = 0;            // == roi.CaptureWidth() at capture time
    int height = 0;           // == roi.CaptureHeight() at capture time
    int strideBytes = 0;      // row pitch in bytes (>= width*4)
    std::vector<uint8_t> pixelsBgra; // strideBytes * height bytes

    // Screen-space origin of captureLocal (0,0), captured at the time this
    // frame was produced (so ROI reconfiguration mid-run stays consistent).
    int captureOriginScreenX = 0;
    int captureOriginScreenY = 0;

    bool Valid() const { return width > 0 && height > 0 && !pixelsBgra.empty(); }
};

// ---------------------------------------------------------------------
// Raw per-frame detection result (roiLocal coordinates)
// ---------------------------------------------------------------------
struct DetectionBand {
    bool present = false;
    int roiLocalTop = 0;
    int roiLocalBottom = 0;
    int roiLocalCenterY = 0;
    float confidence = 0.0f;    // 0..1
    int supportingColumns = 0;
    int heightPx = 0;
};

struct DetectionResult {
    uint64_t frameId = 0;
    TimePoint timestamp{};

    DetectionBand marker;
    DetectionBand target;

    // Where the target border's own color falls on the in-zone<->out-of-
    // zone axis: 0 = fully in-zone color, 1 = fully out-of-zone color,
    // -1 = unknown/not computed (target not present). This is ground
    // truth from the game's own UI, independent of comparing marker vs.
    // target position - see DetectionConfig::targetBorderColor*.
    float targetInZoneColorFraction = -1.0f;

    // Optional debug evidence: one byte per (roiLocalX, roiLocalY) cell,
    // row-major, maskWidth*maskHeight bytes. Only populated when debug
    // mode is enabled (expensive-ish to copy for UI, cheap to compute).
    std::vector<uint8_t> targetMask;
    std::vector<uint8_t> markerMask;
    int maskWidth = 0;
    int maskHeight = 0;
};

// ---------------------------------------------------------------------
// Tracked/predicted band (smoothed, used ONLY for control - never for the
// raw-absence completion timer).
// ---------------------------------------------------------------------
struct TrackedBand {
    bool hasData = false;
    float centerY = 0.0f;       // roiLocal, smoothed/predicted
    float velocityPerSec = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
    float confidence = 0.0f;    // decays while coasting
    int consecutiveMisses = 0;
    bool coasting = false;
};

// ---------------------------------------------------------------------
// Bot state machine
// ---------------------------------------------------------------------
enum class BotState {
    Off,
    Casting,
    Fishing,
    WaitT,
};

inline const char* ToString(BotState s) {
    switch (s) {
        case BotState::Off: return "OFF";
        case BotState::Casting: return "CASTING";
        case BotState::Fishing: return "FISHING";
        case BotState::WaitT: return "WAIT_T";
    }
    return "?";
}

} // namespace fb
