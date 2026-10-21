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
// A drag step with the tuned marker: the radio centre moves (clamped), the tuned (demodulated) frequency stays where it is, and is only
// clamped at the edge of the new passband (edgeFrac of fs around the centre) when the centre would carry it out of the band.
// movesTuned (the setting): the tuned frequency travels with the centre instead.
struct DragResult { double centre, tuned; };
inline DragResult dragWithMarker(double centre, double tuned, double delta, double lo, double hi, double fsMhz, bool movesTuned) {
    const double c = clampCentre(centre + delta, lo, hi);
    if (movesTuned) return {c, tuned + (c - centre)};
    const double e = fsMhz * 0.45;
    return {c, std::min(c + e, std::max(c - e, tuned))};
}
// The view [lo,hi] after the radio centre went from oldC to newC: the span is kept (at most fs). A view still inside the new band stays where
// it is (a drag); otherwise it is centred on the new centre (a click that tuned far away).
struct View { double lo, hi; };
inline View viewAfterRetune(double lo, double hi, double newC, double fsMhz, bool valid) {
    const double bLo = newC - fsMhz / 2, bHi = newC + fsMhz / 2;
    if (!valid || !(hi > lo)) return {bLo, bHi};
    const double span = std::min(fsMhz, hi - lo);
    if (lo >= bLo - 1e-9 && hi <= bHi + 1e-9) return {lo, hi};
    if (lo < bHi && hi > bLo && span < fsMhz) {   // partly overlapping: slide it into the band (a drag that ran to the edge)
        const double l = std::min(bHi - span, std::max(bLo, lo));
        return {l, l + span};
    }
    return {newC - span / 2, newC + span / 2};
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
