// The waterfall at the size it is shown. The history is 4096 columns by 512 rows; drawn straight onto a plot of ~1000 x ~400 pixels, the GPU
// (linear filter, no mipmaps) picked about one texel in four across and some of the rows down, and as the picture slid down by part of a row
// each frame it picked other rows: narrow or short signals blinked. Here each pixel column is the highest level of the columns it covers, and
// the rows go in groups of k (a power of two), the highest of each, so a group is at least a pixel tall: the picture is then only ever
// magnified, and a group keeps its look while it slides. Pure (no ImGui, no GPU): tested in tests/app/test_plot_util.cpp.
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace plt {

struct WfView {
    int c0 = 0, c1 = 0, cols = 0, k = 0, ring = 0;   // source columns [c0, c1) onto cols pixel columns; k rows a group, ring groups kept
    // The texture is drawn with a linear filter, so its edges blend with the texel beyond them: past the last column, and below row
    // ring - 1 where the picture goes on at row 0. Unwritten (black) texels there were a dark line that slid with the picture. So px has
    // one more column (a copy of the last) and one more row (a copy of row 0) wherever the texture has room (W x H).
    int stride = 0, rowsAlloc = 0;
    long long newest = -1, lastR = -1;               // the newest group built, and the newest row it had
    int lutGen = -1;
    const uint8_t* src = nullptr; int srcW = 0, srcH = 0;   // the history it was built from (the W-column one or the full-FFT one)
    std::vector<uint32_t> px;   // rowsAlloc rows of stride colours; group g in row rowOf(g) (newer groups above, wrapping, as the history)
    std::vector<uint8_t> acc;

    // The rows a group needs so that rowsShown rows over heightPx pixels are never squeezed (a power of two, at most H).
    static int groupRows(double rowsShown, double heightPx, int H) {
        int k = 1;
        while (k < H && rowsShown > heightPx * k) k *= 2;
        return k;
    }
    // Groups kept: the newest one may be partly filled, so one more than H / k (which covers H rows)
    static int ringFor(int k, int H) { return k <= 1 ? H : H / k + 1; }
    int rowOf(long long g) const { return ring - 1 - (int)(g % ring); }

    // lv: H rows of W levels (0..255, the index into lut); the newest row, number R (counted from the first), at row wr, older ones at
    // wr+1, wr+2 ... wrapping; have: how many rows there are. Brings px up to date for the columns [nc0, nc1) on ncols pixels in groups of
    // nk rows. full: every row of px changed (upload stride x rowsAlloc); else dirty lists the rows that did. False when nothing changed.
    bool update(const uint8_t* lv, int W, int H, int wr, long long R, int have, const uint32_t* lut, int gen,
                int nc0, int nc1, int ncols, int nk, bool& full, std::vector<int>& dirty) {
        dirty.clear(); full = false;
        nc0 = std::max(0, std::min(W, nc0)); nc1 = std::max(nc0, std::min(W, nc1));
        nk = std::max(1, std::min(H, nk));
        if (nc1 <= nc0 || ncols < 1 || R < 0) return false;
        ncols = std::min(ncols, nc1 - nc0);
        const long long gN = R / nk;
        const bool same = nc0 == c0 && nc1 == c1 && ncols == cols && nk == k && gen == lutGen && newest >= 0 && lv == src && W == srcW && H == srcH;
        if (!same || gN - newest >= ring) {
            c0 = nc0; c1 = nc1; cols = ncols; k = nk; ring = ringFor(k, H); lutGen = gen; src = lv; srcW = W; srcH = H;
            stride = cols + (cols < W ? 1 : 0); rowsAlloc = ring + (ring < H ? 1 : 0);
            px.assign((size_t)rowsAlloc * stride, 0xFF000000u);
            for (long long g = std::max(0LL, gN - ring + 1); g <= gN; g++) build(lv, W, H, wr, R, have, lut, g);
            newest = gN; lastR = R; full = true;
            return true;
        }
        if (R == lastR) return false;
        for (long long g = newest; g <= gN; g++) {
            build(lv, W, H, wr, R, have, lut, g); dirty.push_back(rowOf(g));
            if (rowOf(g) == 0 && rowsAlloc > ring) dirty.push_back(ring);
        }
        newest = gN; lastR = R;
        return true;
    }

private:
    void build(const uint8_t* lv, int W, int H, int wr, long long R, int have, const uint32_t* lut, long long g) {
        const int span = c1 - c0;
        acc.assign((size_t)span, 0);
        bool any = false;
        for (long long r = g * k; r < (g + 1) * k; r++) {
            if (r > R || R - r >= have) continue;
            const uint8_t* s = lv + (size_t)((wr + (R - r)) % H) * W + c0;
            for (int c = 0; c < span; c++) acc[c] = std::max(acc[c], s[c]);
            any = true;
        }
        uint32_t* o = px.data() + (size_t)rowOf(g) * stride;
        if (!any) std::fill(o, o + stride, 0xFF000000u);   // no rows yet: black, as the history starts
        else {
            for (int x = 0; x < cols; x++) {
                const int a = (int)((long long)span * x / cols), b = std::max(a + 1, (int)((long long)span * (x + 1) / cols));
                uint8_t m = acc[a];
                for (int c = a + 1; c < b; c++) m = std::max(m, acc[c]);
                o[x] = lut[m];
            }
            if (stride > cols) o[cols] = o[cols - 1];
        }
        if (rowOf(g) == 0 && rowsAlloc > ring) std::copy(o, o + stride, px.data() + (size_t)ring * stride);
    }
};

} // namespace plt
