// HF digital, JT4: the code (206 tones of 4, the timing), then the whole receiver on synthetic audio (clean, sub-modes A .. G, frequency and
// time offsets, a weak signal, two stations, noise alone). Loopback only: the sync vector is a stand-in (see hfdig_jt4.cpp), so interoperation with
// WSJT-X is not tested.
#include "hfdig_wsjt_testutil.h"
#include "../../core/src/hfdig/hfdig_wsjt_int.h"
#include <chrono>
using namespace wsjttest;
#include "../common/test_util.h"
using namespace dect2::wsjt;

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    const int M = ftx::kJt4;
    {
        const auto t = jt4Tones("CQ K1ABC FN42");
        CHECK(t.size() == 206, "206 tones");
        for (int v : t) CHECK(v >= 0 && v <= 3, "tone range");
        CHECK(jt4Tones("K1ABC W9XYZ -15") != t, "different messages differ");
        CHECK(std::fabs(ftxPeriod(M) - 60.0) < 1e-9 && std::fabs(206 * 2520.0 / 11025 - 47.088) < 0.01, "47.1 s transmission");
    }
    {   // clean, sub-mode A, with a fractional-bin frequency offset
        Imp f;
        const auto d = decode(audio(M, {{"CQ K1ABC FN42", 1501.7, -10, 0.2}}, 2, f, 3), M, f);
        print(d);
        const FtxDecode* x = find(d, "CQ K1ABC FN42");
        CHECK(x && std::fabs(x->hz - 1501.7) < 2.5 && std::fabs(x->dt - 0.2) < 0.1, "clean JT4A");
        CHECK(d.size() == 2, "clean: %d decodes (2 slots)", (int)d.size());
    }
    for (int sub = 0; sub < 7; sub++) {   // the sub-modes, near the threshold, odd offsets
        Imp f; f.sub = sub;
        const auto d = decode(audio(M, {{"K1ABC W9XYZ -12", 900.0 + 13.3 * sub, -16, -0.8 + 0.3 * sub}}, 1, f, 40 + sub), M, f);
        CHECK(find(d, "K1ABC W9XYZ -12"), "JT4%c at -16 dB: not decoded", 'A' + sub);
    }
    {   // a weak one
        Imp f;
        int ok = 0;
        for (int k = 0; k < 4; k++) {
            const auto d = decode(audio(M, {{"CQ DX G4ABC IO91", 1200 + 31.1 * k, -19, 0.1 * k}}, 1, f, 200 + k), M, f);
            if (find(d, "CQ DX G4ABC IO91")) ok++;
            for (const auto& x : d) CHECK(x.msg == "CQ DX G4ABC IO91", "false decode \"%s\"", x.msg.c_str());
        }
        printf("JT4A -19 dB: %d of 4\n", ok);
        CHECK(ok >= 2, "-19 dB: %d of 4", ok);
    }
    {   // two stations
        Imp f;
        const std::vector<St> st = {{"CQ K1ABC FN42", 1000, -13, 0.0}, {"K1ABC W9XYZ -12", 1300, -13, 0.5}};
        const auto d = decode(audio(M, st, 1, f, 12), M, f);
        for (const auto& s : st) CHECK(find(d, s.msg), "\"%s\" not decoded", s.msg.c_str());
    }
    {   // noise alone
        Imp f;
        const auto d = decode(audio(M, {}, 5, f, 77), M, f);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
