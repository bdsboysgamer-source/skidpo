#pragma once

#include <cstdint>

namespace fb {

struct RgbColor {
    uint8_t r = 0, g = 0, b = 0;
};

struct HsvColor {
    float h = 0.0f; // [0, 360)
    float s = 0.0f; // [0, 1]
    float v = 0.0f; // [0, 1]
};

HsvColor RgbToHsv(RgbColor c);

// Marker (bright, low-saturation) likelihood in [0,1], contrasted against
// the local background estimate so it adapts to scene brightness.
float MarkerLikelihood01(RgbColor pixel, RgbColor background);

} // namespace fb
