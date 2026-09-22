#include "ColorModel.h"

#include <cmath>
#include <algorithm>

namespace fb {

namespace {
float Clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }
}

HsvColor RgbToHsv(RgbColor c) {
    float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    float maxC = std::max({r, g, b});
    float minC = std::min({r, g, b});
    float delta = maxC - minC;

    HsvColor out;
    out.v = maxC;
    out.s = (maxC <= 0.0f) ? 0.0f : (delta / maxC);

    if (delta <= 1e-6f) {
        out.h = 0.0f;
    } else if (maxC == r) {
        out.h = 60.0f * std::fmod(((g - b) / delta), 6.0f);
    } else if (maxC == g) {
        out.h = 60.0f * (((b - r) / delta) + 2.0f);
    } else {
        out.h = 60.0f * (((r - g) / delta) + 4.0f);
    }
    if (out.h < 0.0f) out.h += 360.0f;
    return out;
}

float MarkerLikelihood01(RgbColor pixel, RgbColor background) {
    HsvColor hsv = RgbToHsv(pixel);
    HsvColor bgHsv = RgbToHsv(background);

    float brightnessScore = Clamp01((hsv.v - 0.55f) / 0.45f);
    float neutralScore = Clamp01(1.0f - hsv.s / 0.35f);
    float contrastVsBg = Clamp01((hsv.v - bgHsv.v) / 0.30f);

    return brightnessScore * 0.40f + neutralScore * 0.35f + contrastVsBg * 0.25f;
}

} // namespace fb
