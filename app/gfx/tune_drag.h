// Pure decisions for dragging the spectrum and for the two click-tuning styles (tests/app/test_tune_drag.cpp).
#pragma once
#include <algorithm>
#include <cmath>
namespace tunedrag {
// a press is a click (tunes) while it moves under 4 px; a slow still press is still a click; a move of 4 px or more is a drag (pans)
inline bool isDrag(double dxPx, double dyPx, double heldSeconds) {
    (void)heldSeconds;
    return std::fabs(dxPx) >= 4 || std::fabs(dyPx) >= 4;
}
// the radio centre may not leave the radio's range
inline double clampCentre(double centreMhz, double lo, double hi) { return std::min(hi, std::max(lo, centreMhz)); }
// A drag that ran past the captured band by overflowMhz: add it to pending, and hand out the retune step at most every minInterval seconds
// (coalesced: one step carries everything that piled up, never a queue). Returns the delta to retune by, 0 for none yet.
struct Pacer {
    double pending = 0, last = -1e9;
    double step(double overflowMhz, double now, double minInterval = 0.07) {
        pending += overflowMhz;
        if (pending == 0 || now - last < minInterval) return 0;
        const double d = pending; pending = 0; last = now; return d;
    }
};
// the new radio centre for a drag step: clamped to the radio's range; the step actually applied comes back through applied
inline double dragCentre(double centreMhz, double delta, double lo, double hi, double& applied) {
    const double c = clampCentre(centreMhz + delta, lo, hi); applied = c - centreMhz; return c;
}
struct Click { bool retune; double centreMhz; bool decodeOnly; };
// What a click on absMhz does. centreStyle: the target button (centre the radio on the click); else the click only moves the marker
// inside the captured band and retunes only when it lies outside it. noRetune: a file or a wide mode: the radio never moves.
inline Click clickPlan(bool centreStyle, bool noRetune, double absMhz, double centreMhz, double fsMhz, double lo, double hi) {
    if (noRetune) return {false, centreMhz, true};
    const bool inside = std::fabs(absMhz - centreMhz) < fsMhz * 0.45;
    if (!centreStyle && inside) return {false, centreMhz, true};
    return {true, clampCentre(absMhz, lo, hi), false};
}
}
