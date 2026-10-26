// DVB-CID: the frame coding (CRC-8, BCH(111,69) with six corrections, scrambler, unique word, content fields), the chip sequence, and the receiver on a synthetic
// signal under noise: acquisition, tracking with a frequency offset and a sample clock error, the four copies combined, the identifier and fields read.
#include "dect2/dvbs/dvbs_cid.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2;
using namespace dect2::dvbcid;
#include "../common/test_util.h"

static void addNoise(std::vector<cf32>& x, double snrDb, uint32_t seed) {
    std::mt19937 g(seed);
    std::normal_distribution<float> n(0.f, 1.f);
    const float sd = (float)std::sqrt(std::pow(10.0, -snrDb / 10.0) / 2.0);
    for (auto& v : x) v += cf32(n(g) * sd, n(g) * sd);
}

int main() {
    // chip sequence
    {
        const auto& c = chipSequence();
        uint32_t first = 0;
        for (int i = 0; i < 32; i++) first = (first << 1) | c[(size_t)i];
        CHECK(first == 0x5091E364u, "first 32 chips %08X", first);
        CHECK((int)c.size() == kChips, "4096 chips");
    }
    // identifier text
    {
        const uint64_t g = 0x0123456789ABCDEFull;
        const std::string t = guidText(g);
        CHECK(t.size() == 26 && t.substr(3) == "01:23:45:67:89:AB:CD:EF", "guid text %s", t.c_str());
        CHECK(guidMac(0x001122FFFE334455ull) == "00:11:22:33:44:55", "mac %s", guidMac(0x001122FFFE334455ull).c_str());
        CHECK(guidMac(g).empty(), "no mac");
    }
    // frame coding, error correction
    const uint64_t guid = 0x1A2B3C4D5E6F7081ull;
    Report rep;
    rep.setPosition(54.6872, 25.2797);
    rep.setTelephone("+370 600 12345 ext 7");
    rep.setUserText("Eye of ZorCom test uplink");
    const auto pairs = rep.frameFields();
    CHECK(pairs.size() == 6, "%zu frames of fields", pairs.size());
    {
        const Field f[2] = {pairs[0][0], pairs[0][1]};
        std::vector<uint8_t> fr = buildFrame(guid, f, false);
        Frame o;
        CHECK((int)fr.size() == kFrameBits && parseFrame(fr.data(), o) && o.guid == guid && !o.second && o.fields[0].contentId == f[0].contentId && o.fields[1].info == f[1].info, "clean frame");
        // errors: up to six per half are corrected, seven are not
        std::mt19937 g(5);
        for (int errs = 1; errs <= 7; errs++) {
            int okCount = 0;
            for (int trial = 0; trial < 40; trial++) {
                std::vector<uint8_t> b = fr;
                for (int h = 0; h < 2; h++) {
                    std::vector<int> idx;
                    while ((int)idx.size() < errs) { const int i = kUwBits + h * kHalf + (int)(g() % kHalf); bool dup = false; for (int j : idx) dup |= j == i; if (!dup) idx.push_back(i); }
                    for (int i : idx) b[(size_t)i] ^= 1;
                }
                Frame r;
                if (parseFrame(b.data(), r) && r.guid == guid) okCount++;
            }
            if (errs <= 6) CHECK(okCount == 40, "%d errors per half: %d of 40 decoded", errs, okCount);
            else CHECK(okCount == 0, "7 errors per half decoded %d times", okCount);
        }
        std::vector<uint8_t> second = buildFrame(guid, f, true);
        Frame o2;
        CHECK(parseFrame(second.data(), o2) && o2.second, "complemented unique word");
    }
    // report fields
    {
        Report r2;
        for (const auto& p : pairs) { Frame fr; fr.guid = guid; fr.fields[0] = p[0]; fr.fields[1] = p[1]; r2.add(fr); }
        double la = 0, lo = 0;
        CHECK(r2.latitude(la) && std::fabs(la - 54.6872) < 1e-3 && r2.longitude(lo) && std::fabs(lo - 25.2797) < 1e-3, "position %.4f %.4f", la, lo);
        CHECK(r2.telephone() == "+37060012345 ext. 7", "telephone '%s'", r2.telephone().c_str());
        CHECK(r2.userText() == "Eye of ZorCom test uplink", "text '%s'", r2.userText().c_str());
        CHECK(r2.haveGuid && r2.guid == guid, "guid");
    }
    // the receiver
    const double cr = 112e3;
    struct Case { double snrDb, freq, ppm; const char* name; };
    const Case cases[] = {{-34.0, 220.0, 0.0, "nominal"}, {-34.0, 305.0, 25.0, "frequency and clock error"}, {-37.0, 150.0, -10.0, "weak"}};
    for (const Case& c : cases) {
        std::vector<std::vector<uint8_t>> frames;
        {
            const Field f[2] = {pairs[0][0], pairs[0][1]};
            frames.push_back(buildFrame(guid, f, false));
        }
        uint8_t diff = 0;
        std::vector<uint8_t> chips = spreadFrames(frames, diff);
        chips.resize((size_t)3 * kFrameBits * kChips);   // three of the four copies keep the test signal at about 190 MB
        std::vector<cf32> x = modulate(chips, c.freq, cr, c.ppm, 0.7);
        addNoise(x, c.snrDb, 11);
        Rx rx(cr, false);
        std::vector<Frame> got;
        for (size_t at = 0; at < x.size(); at += 65536) rx.push(x.data() + at, std::min<size_t>(65536, x.size() - at), got);
        const Stats st = rx.stats();
        printf("%s: acquired %d, offset %.1f Hz, SNR %.1f dB, %llu bits, %zu frames, clock %.1f ppm\n", c.name, (int)st.acquired, st.offsetHz, st.snrDb, (unsigned long long)st.bits, got.size(), st.clockPpm);
        CHECK(st.acquired, "%s: acquired", c.name);
        CHECK(got.size() >= 1 && got[0].guid == guid, "%s: %zu frames, first guid %016llX", c.name, got.size(), got.empty() ? 0ull : (unsigned long long)got[0].guid);
        if (!got.empty()) CHECK(std::fabs(st.offsetHz - c.freq) < 3.0, "%s: offset %.1f Hz, wanted %.1f", c.name, st.offsetHz, c.freq);
    }
    return testSummary("dvbs cid");
}
