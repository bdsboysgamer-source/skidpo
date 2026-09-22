#include "Detector.h"
#include "ColorModel.h"
#include "CandidateScoring.h"

#include <algorithm>
#include <vector>
#include <cmath>

namespace fb {

namespace {

float Clamp01(float v) { return std::max(0.0f, std::min(1.0f, v)); }

inline RgbColor ReadPixelBgra(const CapturedFrame& frame, int captureLocalX, int captureLocalY) {
    captureLocalX = std::clamp(captureLocalX, 0, frame.width - 1);
    captureLocalY = std::clamp(captureLocalY, 0, frame.height - 1);
    const uint8_t* row = frame.pixelsBgra.data() + static_cast<size_t>(captureLocalY) * frame.strideBytes;
    const uint8_t* px = row + static_cast<size_t>(captureLocalX) * 4;
    // DXGI_FORMAT_B8G8R8A8_UNORM byte order: B, G, R, A
    return RgbColor{ px[2], px[1], px[0] };
}

// Average color of the left+right background flanks at a given capture row.
RgbColor EstimateRowBackground(const CapturedFrame& frame, const RoiConfig& roi, int captureLocalY) {
    int rSum = 0, gSum = 0, bSum = 0, n = 0;
    for (int x = 0; x < roi.flankPixels; ++x) {
        RgbColor c = ReadPixelBgra(frame, x, captureLocalY);
        rSum += c.r; gSum += c.g; bSum += c.b; ++n;
    }
    int rightStart = roi.flankPixels + roi.width;
    for (int x = 0; x < roi.flankPixels; ++x) {
        RgbColor c = ReadPixelBgra(frame, rightStart + x, captureLocalY);
        rSum += c.r; gSum += c.g; bSum += c.b; ++n;
    }
    if (n == 0) return RgbColor{0, 0, 0};
    return RgbColor{
        static_cast<uint8_t>(rSum / n),
        static_cast<uint8_t>(gSum / n),
        static_cast<uint8_t>(bSum / n)
    };
}

float ComponentConfidence(const Component& c, int roiWidth) {
    float coverageFraction = Clamp01(static_cast<float>(c.distinctColumns) / static_cast<float>(std::max(1, roiWidth)));
    float edgeScore = Clamp01(c.edgeContrast / 0.30f);
    return Clamp01(c.avgScore * 0.40f
                  + c.fillRatio * 0.20f
                  + coverageFraction * 0.25f
                  + edgeScore * 0.15f);
}

DetectionBand BestBandFromComponents(const std::vector<Component>& components, int roiWidth,
                                      float minConfidenceToReport) {
    DetectionBand band;
    const Component* best = nullptr;
    float bestConfidence = 0.0f;

    for (const Component& c : components) {
        float conf = ComponentConfidence(c, roiWidth);
        if (conf > bestConfidence) {
            bestConfidence = conf;
            best = &c;
        }
    }

    if (best != nullptr && bestConfidence >= minConfidenceToReport) {
        band.present = true;
        band.roiLocalTop = best->rowMin;
        band.roiLocalBottom = best->rowMax;
        band.roiLocalCenterY = (best->rowMin + best->rowMax) / 2;
        band.confidence = bestConfidence;
        band.supportingColumns = best->distinctColumns;
        band.heightPx = best->rowMax - best->rowMin + 1;
    }
    return band;
}

float RgbEuclidean(RgbColor a, RgbColor b) {
    float dr = static_cast<float>(a.r) - static_cast<float>(b.r);
    float dg = static_cast<float>(a.g) - static_cast<float>(b.g);
    float db = static_cast<float>(a.b) - static_cast<float>(b.b);
    return std::sqrt(dr * dr + dg * dg + db * db);
}

// A vertical run of near-identical color in a single capture column.
struct ColorRun {
    int start;
    int end; // inclusive
    RgbColor avgColor;
    int Length() const { return end - start + 1; }
};

// Greedily walks the column top to bottom, extending a run while each new
// pixel stays within `uniformityTolerance` of the run's running average.
// The target border renders as a genuinely flat (non-gradient) run of
// pixels, unlike the semi-transparent fill, so this cleanly separates it
// from its neighbors without needing per-pixel color classification.
std::vector<ColorRun> FindUniformRuns(const std::vector<RgbColor>& column, float uniformityTolerance) {
    std::vector<ColorRun> runs;
    int n = static_cast<int>(column.size());
    int i = 0;
    while (i < n) {
        long sumR = column[i].r, sumG = column[i].g, sumB = column[i].b;
        int count = 1;
        int j = i;
        while (j + 1 < n) {
            RgbColor runAvg{
                static_cast<uint8_t>(sumR / count),
                static_cast<uint8_t>(sumG / count),
                static_cast<uint8_t>(sumB / count)
            };
            if (RgbEuclidean(column[j + 1], runAvg) > uniformityTolerance) break;
            ++j;
            sumR += column[j].r; sumG += column[j].g; sumB += column[j].b; ++count;
        }
        runs.push_back(ColorRun{
            i, j,
            RgbColor{ static_cast<uint8_t>(sumR / count), static_cast<uint8_t>(sumG / count), static_cast<uint8_t>(sumB / count) }
        });
        i = j + 1;
    }
    return runs;
}

// Projects `color` onto the line between `colorA` and `colorB` in RGB
// space. Returns the projection parameter t (0 = colorA, 1 = colorB, may
// fall outside [0,1]) and the perpendicular (off-axis) distance.
struct AxisProjection { float t; float perpDist; };

AxisProjection ProjectOntoColorAxis(RgbColor color, RgbColor colorA, RgbColor colorB) {
    float dr = static_cast<float>(colorB.r) - colorA.r;
    float dg = static_cast<float>(colorB.g) - colorA.g;
    float db = static_cast<float>(colorB.b) - colorA.b;
    float lenSq = dr * dr + dg * dg + db * db;

    float cr = static_cast<float>(color.r) - colorA.r;
    float cg = static_cast<float>(color.g) - colorA.g;
    float cb = static_cast<float>(color.b) - colorA.b;

    AxisProjection result{0.0f, 0.0f};
    if (lenSq <= 1e-6f) {
        result.perpDist = std::sqrt(cr * cr + cg * cg + cb * cb);
        return result;
    }

    float t = (cr * dr + cg * dg + cb * db) / lenSq;
    float projR = colorA.r + t * dr;
    float projG = colorA.g + t * dg;
    float projB = colorA.b + t * db;
    float pr = static_cast<float>(color.r) - projR;
    float pg = static_cast<float>(color.g) - projG;
    float pb = static_cast<float>(color.b) - projB;

    result.t = t;
    result.perpDist = std::sqrt(pr * pr + pg * pg + pb * pb);
    return result;
}

} // namespace

DetectionResult Detector::Detect(const CapturedFrame& frame, bool computeDebugMasks,
                                  const TargetPriorHint& targetPrior) const {
    DetectionResult result;
    result.frameId = frame.frameId;
    result.timestamp = frame.timestamp;

    const RoiConfig& roi = m_config.roi;
    const DetectionConfig& det = m_config.detection;

    if (!frame.Valid() || frame.width != roi.CaptureWidth() || frame.height != roi.CaptureHeight()) {
        // Frame doesn't match the expected capture geometry (e.g. mid
        // reconfiguration, or an init frame) - report nothing rather than
        // guessing at wrong coordinates.
        return result;
    }

    // ------------------------------------------------------------------
    // TARGET: border-run detection.
    //
    // The target zone's border is a SOLID line, not alpha-blended with
    // the 3D world behind the bar - unlike the fill, which testing showed
    // is unreliable to classify (its resting/unlit tint can resemble the
    // lit tint depending on background). The border's straight vertical
    // segment (away from where it curves into the zone's horizontal cap
    // lines) is a run of near-identical pixels, and its own color fades
    // between two known endpoints depending on whether the marker is
    // currently inside the zone - so it doubles as ground truth for "in
    // zone right now" once found.
    //
    // The GUI is static, so the border's column is a known fixed location
    // (not searched for) - only a couple of neighboring columns are also
    // checked, purely for redundancy against a transient capture glitch
    // on any single column.
    {
        RgbColor colorA{ det.targetBorderColorInZone.r, det.targetBorderColorInZone.g, det.targetBorderColorInZone.b };
        RgbColor colorB{ det.targetBorderColorOutOfZone.r, det.targetBorderColorOutOfZone.g, det.targetBorderColorOutOfZone.b };

        int bestRunStart = -1, bestRunEnd = -1;
        float bestScore = 0.0f;
        AxisProjection bestProjection{0.0f, 0.0f};

        int colStart = std::max(0, det.targetBorderColumnRoiLocalX);
        int colEnd = std::min(roi.width - 1, colStart + std::max(1, det.targetBorderColumnSearchWidth) - 1);

        float priorRadius = targetPrior.valid
            ? std::max(det.targetPriorSearchMinRadiusPx, targetPrior.maxDistancePx)
            : 0.0f;

        for (int roiLocalX = colStart; roiLocalX <= colEnd; ++roiLocalX) {
            int captureLocalX = RoiLocalXToCaptureLocalX(roi, roiLocalX);
            std::vector<RgbColor> column(roi.height);
            for (int y = 0; y < roi.height; ++y) column[y] = ReadPixelBgra(frame, captureLocalX, y);

            std::vector<ColorRun> runs = FindUniformRuns(column, det.targetBorderUniformityTolerance);
            for (const ColorRun& run : runs) {
                int lengthDiff = std::abs(run.Length() - det.targetBorderRunHeightPx);
                if (lengthDiff > det.targetBorderRunHeightTolerancePx) continue;

                AxisProjection proj = ProjectOntoColorAxis(run.avgColor, colorA, colorB);
                float lengthScore = Clamp01(1.0f - static_cast<float>(lengthDiff) / std::max(1, det.targetBorderRunHeightTolerancePx));
                float axisFitScore = Clamp01(1.0f - proj.perpDist / std::max(1.0f, det.targetBorderColorAxisTolerance));

                float score;
                if (targetPrior.valid) {
                    // Spatially-informed: a structurally-correct run near
                    // where the target was last seen is trusted even if
                    // its color has drifted well past
                    // targetBorderColorAxisTolerance (a strongly-colored
                    // background, e.g. deep blue, was found to shift it
                    // ~5x past that tolerance) - color only boosts
                    // confidence here, it doesn't gate acceptance. The
                    // target can only move a little between two
                    // consecutive frames, so proximity to the prior is
                    // the substitute discriminator.
                    float runCenterY = static_cast<float>(run.start + run.end) * 0.5f;
                    float priorDist = std::fabs(runCenterY - targetPrior.roiLocalCenterY);
                    if (priorDist > priorRadius) continue;
                    float priorProximityScore = Clamp01(1.0f - priorDist / priorRadius);
                    score = lengthScore * priorProximityScore * (0.5f + 0.5f * axisFitScore);
                } else {
                    // No usable prior (re-acquiring after a genuine
                    // absence) - color match against the known axis is
                    // the only available discriminator, so it's a hard
                    // gate here, same as before.
                    if (proj.perpDist > det.targetBorderColorAxisTolerance) continue;
                    if (proj.t < -det.targetBorderColorAxisMargin || proj.t > 1.0f + det.targetBorderColorAxisMargin) continue;
                    score = lengthScore * axisFitScore;
                }

                if (score > bestScore) {
                    bestScore = score;
                    bestRunStart = run.start;
                    bestRunEnd = run.end;
                    bestProjection = proj;
                }
            }
        }

        if (bestRunStart >= 0 && bestScore >= det.minConfidenceToReport) {
            result.target.present = true;
            result.target.roiLocalTop = bestRunStart;
            result.target.roiLocalBottom = bestRunEnd;
            result.target.roiLocalCenterY = (bestRunStart + bestRunEnd) / 2;
            result.target.confidence = bestScore;
            result.target.supportingColumns = colEnd - colStart + 1;
            result.target.heightPx = bestRunEnd - bestRunStart + 1;
            // Only trust the in-zone/out-of-zone color reading itself when
            // the winning run's color is reasonably close to the known
            // axis - a prior-assisted match can win on structure+proximity
            // alone with a poor color fit, and reporting a t from far off
            // the axis would be a meaningless number dressed up as data.
            result.targetInZoneColorFraction = (bestProjection.perpDist <= det.targetBorderColorAxisTolerance)
                ? Clamp01(bestProjection.t) : -1.0f;
        }
    }

    // ------------------------------------------------------------------
    // MARKER: brightness/neutrality vs. local flank background. Validated
    // as working reliably as-is - unchanged.
    // ------------------------------------------------------------------
    std::vector<RgbColor> rowBackground(roi.height);
    for (int y = 0; y < roi.height; ++y) {
        rowBackground[y] = EstimateRowBackground(frame, roi, y);
    }

    ScoreGrid markerGrid{roi.width, roi.height, std::vector<float>(static_cast<size_t>(roi.width) * roi.height)};
    for (int roiLocalX = 0; roiLocalX < roi.width; ++roiLocalX) {
        int captureLocalX = RoiLocalXToCaptureLocalX(roi, roiLocalX);
        for (int roiLocalY = 0; roiLocalY < roi.height; ++roiLocalY) {
            RgbColor pixel = ReadPixelBgra(frame, captureLocalX, roiLocalY);
            const RgbColor& bg = rowBackground[roiLocalY];
            float markerScore = MarkerLikelihood01(pixel, bg);
            markerGrid.Set(roiLocalX, roiLocalY, markerScore);
        }
    }

    auto markerMask = ThresholdAndBridge(markerGrid, det.markerMinPixelScore, det.markerGapBridgePx);
    auto markerComponents = FindComponents(markerGrid, markerMask, det.markerMinHeightPx,
                                            det.markerMinColumns, det.markerMinFillRatio);
    result.marker = BestBandFromComponents(markerComponents, roi.width, det.minConfidenceToReport);

    if (computeDebugMasks) {
        result.maskWidth = roi.width;
        result.maskHeight = roi.height;
        result.markerMask.resize(markerMask.size());
        for (size_t i = 0; i < markerMask.size(); ++i) result.markerMask[i] = markerMask[i] ? 255 : 0;

        // The target mask is synthesized for visualization only (the real
        // detector only examines a couple of columns, not the full
        // width) - it marks the detected band across the whole bar width
        // so the debug overlay's evidence strip still shows something
        // honest and readable.
        result.targetMask.assign(static_cast<size_t>(roi.width) * roi.height, 0);
        if (result.target.present) {
            for (int y = result.target.roiLocalTop; y <= result.target.roiLocalBottom; ++y) {
                for (int x = 0; x < roi.width; ++x) {
                    result.targetMask[static_cast<size_t>(y) * roi.width + x] = 255;
                }
            }
        }
    }

    return result;
}

} // namespace fb
