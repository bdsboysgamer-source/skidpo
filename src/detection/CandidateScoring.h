#pragma once

#include <vector>
#include <cstdint>

namespace fb {

// A dense per-pixel score grid over the ROI-local bar area (roiLocalX in
// [0,width), roiLocalY in [0,height)). Row-major: index = y*width + x.
struct ScoreGrid {
    int width = 0;
    int height = 0;
    std::vector<float> score;

    float At(int x, int y) const { return score[static_cast<size_t>(y) * width + x]; }
    void Set(int x, int y, float v) { score[static_cast<size_t>(y) * width + x] = v; }
};

// A connected region of "target-like" or "marker-like" cells, described in
// roiLocal coordinates plus geometric/quality evidence used for scoring.
struct Component {
    int rowMin = 0, rowMax = 0;   // roiLocalY bounds, inclusive
    int colMin = 0, colMax = 0;   // roiLocalX bounds, inclusive
    int cellCount = 0;            // number of mask-true cells belonging to this component
    int distinctColumns = 0;      // number of distinct roiLocalX columns touched
    float avgScore = 0.0f;        // mean raw score of member cells
    float fillRatio = 0.0f;       // cellCount / (bounding box area)
    float edgeContrast = 0.0f;    // score drop just outside rowMin/rowMax vs inside
    float confidence = 0.0f;      // final composite confidence in [0,1], set by caller
};

// Builds a binary mask from a continuous score grid: a cell is true if its
// score meets `threshold`. Then bridges small vertical gaps (<= gapPx rows,
// in the same column) between true cells to compensate for transparency-
// induced fragmentation, without merging components separated by a larger
// gap. Returns a width*height 0/1 byte mask (post-bridging).
std::vector<uint8_t> ThresholdAndBridge(const ScoreGrid& grid, float threshold, int gapPx);

// Finds connected components in the bridged mask (a cell connects to
// same-column neighbors and to adjacent-column neighbors within 1 row),
// and returns those meeting the minimum geometric requirements. Confidence
// is NOT set by this function (see Detector, which combines geometry with
// color-model evidence); rowMin/rowMax/colMin/colMax/cellCount/
// distinctColumns/avgScore/fillRatio/edgeContrast are all populated.
std::vector<Component> FindComponents(const ScoreGrid& grid, const std::vector<uint8_t>& mask,
                                       int minHeightPx, int minColumns, float minFillRatio);

} // namespace fb
