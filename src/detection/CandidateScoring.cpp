#include "CandidateScoring.h"

#include <algorithm>
#include <vector>

namespace fb {

std::vector<uint8_t> ThresholdAndBridge(const ScoreGrid& grid, float threshold, int gapPx) {
    const int w = grid.width, h = grid.height;
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);

    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            mask[static_cast<size_t>(y) * w + x] = (grid.At(x, y) >= threshold) ? 1 : 0;
        }
    }

    if (gapPx <= 0) return mask;

    // Per-column vertical closing: fill a run of `false` cells of length
    // <= gapPx if it is bounded above and below (within the column) by
    // `true` cells. This bridges fragments caused by the semi-transparent
    // overlay's uneven blending without merging genuinely separate regions.
    for (int x = 0; x < w; ++x) {
        int y = 0;
        while (y < h) {
            if (mask[static_cast<size_t>(y) * w + x] == 0) {
                int gapStart = y;
                while (y < h && mask[static_cast<size_t>(y) * w + x] == 0) ++y;
                int gapLen = y - gapStart;
                bool boundedAbove = gapStart > 0;
                bool boundedBelow = y < h;
                if (boundedAbove && boundedBelow && gapLen <= gapPx) {
                    for (int fy = gapStart; fy < y; ++fy) {
                        mask[static_cast<size_t>(fy) * w + x] = 1;
                    }
                }
            } else {
                ++y;
            }
        }
    }

    return mask;
}

std::vector<Component> FindComponents(const ScoreGrid& grid, const std::vector<uint8_t>& mask,
                                       int minHeightPx, int minColumns, float minFillRatio) {
    const int w = grid.width, h = grid.height;
    std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
    std::vector<Component> results;

    // Neighbor connectivity: same or adjacent column, within 1 row. This
    // treats horizontally-adjacent bar columns as connected even if not
    // pixel-identical rows, which is what lets a genuine target region
    // register as one coherent component across the bar's width.
    static const int dx[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    static const int dy[8] = {-1, 0, 1, -1, 1, -1, 0, 1};

    std::vector<int> stackX, stackY;

    for (int sx = 0; sx < w; ++sx) {
        for (int sy = 0; sy < h; ++sy) {
            size_t sidx = static_cast<size_t>(sy) * w + sx;
            if (mask[sidx] == 0 || visited[sidx]) continue;

            stackX.clear();
            stackY.clear();
            stackX.push_back(sx);
            stackY.push_back(sy);
            visited[sidx] = 1;

            int rowMin = sy, rowMax = sy, colMin = sx, colMax = sx;
            int cellCount = 0;
            double scoreSum = 0.0;
            std::vector<uint8_t> columnTouched(w, 0);

            while (!stackX.empty()) {
                int cx = stackX.back(); int cy = stackY.back();
                stackX.pop_back(); stackY.pop_back();

                size_t cidx = static_cast<size_t>(cy) * w + cx;
                ++cellCount;
                scoreSum += grid.At(cx, cy);
                columnTouched[cx] = 1;
                rowMin = std::min(rowMin, cy);
                rowMax = std::max(rowMax, cy);
                colMin = std::min(colMin, cx);
                colMax = std::max(colMax, cx);

                for (int k = 0; k < 8; ++k) {
                    int nx = cx + dx[k];
                    int ny = cy + dy[k];
                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                    size_t nidx = static_cast<size_t>(ny) * w + nx;
                    if (mask[nidx] == 0 || visited[nidx]) continue;
                    visited[nidx] = 1;
                    stackX.push_back(nx);
                    stackY.push_back(ny);
                }
                (void)cidx;
            }

            int heightPx = rowMax - rowMin + 1;
            int distinctColumns = 0;
            for (int c = 0; c < w; ++c) distinctColumns += columnTouched[c];

            int boxArea = heightPx * (colMax - colMin + 1);
            float fillRatio = boxArea > 0 ? static_cast<float>(cellCount) / static_cast<float>(boxArea) : 0.0f;

            if (heightPx < minHeightPx || distinctColumns < minColumns || fillRatio < minFillRatio) {
                continue;
            }

            // Edge contrast: how much the score drops just outside the
            // component's vertical extent (at the component's horizontal
            // center column) compared to just inside it.
            int centerCol = (colMin + colMax) / 2;
            float insideScore = grid.At(centerCol, std::clamp((rowMin + rowMax) / 2, 0, h - 1));
            float outsideAboveScore = (rowMin - 1 >= 0) ? grid.At(centerCol, rowMin - 1) : 0.0f;
            float outsideBelowScore = (rowMax + 1 < h) ? grid.At(centerCol, rowMax + 1) : 0.0f;
            float edgeContrast = std::max(0.0f, insideScore - std::max(outsideAboveScore, outsideBelowScore));

            Component comp;
            comp.rowMin = rowMin; comp.rowMax = rowMax;
            comp.colMin = colMin; comp.colMax = colMax;
            comp.cellCount = cellCount;
            comp.distinctColumns = distinctColumns;
            comp.avgScore = static_cast<float>(scoreSum / std::max(1, cellCount));
            comp.fillRatio = fillRatio;
            comp.edgeContrast = edgeContrast;
            comp.confidence = 0.0f;
            results.push_back(comp);
        }
    }

    return results;
}

} // namespace fb
