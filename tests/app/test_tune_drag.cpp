#include "../../app/gfx/tune_drag.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    using namespace tunedrag;
    check(!isDrag(3, 3, 0.1), "small move is a click");
    check(isDrag(4, 0, 0.1), "4 px is a drag");
    check(!isDrag(0, 0, 2.0), "slow still press is a click");
    check(clampCentre(2000, 1, 1766) == 1766 && clampCentre(0, 1, 1766) == 1, "clamp");
    Pacer p;
    check(p.step(0.5, 1.0) == 0.5, "first step passes");
    check(p.step(0.2, 1.01) == 0 && p.step(0.1, 1.02) == 0, "rate limited");
    const double d = p.step(0, 1.2);
    check(std::fabs(d - 0.3) < 1e-12, "coalesced");
    check(p.step(0, 2.0) == 0, "nothing pending");
    double ap;
    const double c = dragCentre(1765.9, 0.5, 1, 1766, ap);
    check(c == 1766 && std::fabs(ap - 0.1) < 1e-9, "drag clamps to the radio limit");
    Click k = clickPlan(false, false, 100.1, 100.0, 2.0, 1, 1766);
    check(!k.retune && k.decodeOnly, "normal inside: marker only");
    k = clickPlan(false, false, 105.0, 100.0, 2.0, 1, 1766);
    check(k.retune && k.centreMhz == 105.0, "normal outside: retune");
    k = clickPlan(true, false, 100.1, 100.0, 2.0, 1, 1766);
    check(k.retune && k.centreMhz == 100.1, "centre style retunes even inside");
    k = clickPlan(true, false, 5000, 100.0, 2.0, 1, 1766);
    check(k.centreMhz == 1766, "centre style clamps");
    k = clickPlan(true, true, 100.4, 100.0, 2.0, 1, 1766);
    check(!k.retune && k.decodeOnly, "file/wide never retunes");
    DragResult r = dragWithMarker(100.0, 100.5, 0.4, 1, 1766, 2.0, false);
    check(r.centre == 100.4 && r.tuned == 100.5, "marker keeps its frequency while inside the band");
    r = dragWithMarker(100.0, 100.5, 2.0, 1, 1766, 2.0, false);
    check(r.centre == 102.0 && std::fabs(r.tuned - 101.1) < 1e-9, "marker clamps at the passband edge, no jump");
    r = dragWithMarker(100.0, 100.5, 0.4, 1, 1766, 2.0, true);
    check(std::fabs(r.tuned - 100.9) < 1e-9, "setting: tuned frequency moves with the centre");
    r = dragWithMarker(1765.9, 1766.0, 0.5, 1, 1766, 2.0, true);
    check(r.centre == 1766 && std::fabs(r.tuned - 1766.1) < 1e-9, "moving marker follows only the applied step");
    View v = viewAfterRetune(100.2, 100.4, 100.1, 2.0, true);
    check(v.lo == 100.2 && v.hi == 100.4, "zoomed view inside the new band stays (span kept)");
    v = viewAfterRetune(500, 500.5, 100, 2.0, true);
    check(std::fabs((v.hi - v.lo) - 0.5) < 1e-9 && std::fabs((v.lo + v.hi) / 2 - 100) < 1e-9, "far retune: span kept, view centred");
    v = viewAfterRetune(0, 0, 100, 2.0, false);
    check(v.lo == 99 && v.hi == 101, "no view yet: whole band");
    return fails ? 1 : 0;
}
