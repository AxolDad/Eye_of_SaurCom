// TETRA voice path: an unencrypted generated traffic burst reaches the speech frame sink as the right coded frame; an
// encrypted one, and one with unknown encryption state, produce "not decoded" and the sink is never called.
#include "dect2/tetra/tetra_phy.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/tetra/tetra_voice_codec.h"
#include <cstdio>
#include <cstring>
#include <random>
using namespace dect2::tetra;
#include "../common/test_util.h"

struct Sink : SpeechFrameSink {
    int calls = 0; Bits last;
    void onCodedFrame(const uint8_t* b, size_t n) override { calls++; last.assign(b, b + n); }
};

// A generated burst: the 432 type-4 bits are scrambled the way the transmitter does, then made soft. (The speech channel interleaver is
// the traffic decoder's business, see test_tetra_traffic.cpp; the voice path hands the descrambled type-4 bits on.)
static void makeBurst(const Bits& t3, uint32_t scr, std::vector<float>& l) {
    Bits t5 = t3;
    scramble(t5.data(), 432, scr);
    l.resize(432);
    for (int i = 0; i < 432; i++) l[(size_t)i] = t5[(size_t)i] ? 1.f : -1.f;
}

int main() {
    std::mt19937 g(9);
    Bits t3(432);
    for (auto& v : t3) v = (uint8_t)(g() & 1);
    const uint32_t scr = scramblerInit(262, 1, 5);
    std::vector<float> l;
    makeBurst(t3, scr, l);

    {   // unencrypted: the coded frame reaches the sink unchanged
        Sink s; CallEncryption e; e.known = true; e.encMode = 0;
        const VoiceVerdict v = processTrafficBurst(l.data(), l.data() + 216, scr, e, &s);
        CHECK(v == VoiceVerdict::Passed, "clear burst passes");
        CHECK(s.calls == 1 && s.last == t3, "clear burst: coded frame delivered intact");
        CHECK(std::strcmp(voiceVerdictText(v), "voice (clear)") == 0, "clear text");
    }
    {   // encrypted: not decoded, sink untouched
        Sink s; CallEncryption e; e.known = true; e.encMode = 1;
        const VoiceVerdict v = processTrafficBurst(l.data(), l.data() + 216, scr, e, &s);
        CHECK(v == VoiceVerdict::EncryptedNotDecoded, "encrypted burst refused");
        CHECK(s.calls == 0, "encrypted burst: sink never called");
        CHECK(std::strcmp(voiceVerdictText(v), "encrypted, not decoded") == 0, "encrypted text");
    }
    {   // a cell that only advertises air interface encryption, with a call known clear: decoded; encrypted evidence still refuses
        Sink s; CallEncryption e; e.known = true; e.cellOffersEncryption = true;
        CHECK(processTrafficBurst(l.data(), l.data() + 216, scr, e, &s) == VoiceVerdict::Passed, "advertising cell, clear call: decoded");
        Sink s2; CallEncryption e2; e2.known = true; e2.cellOffersEncryption = true; e2.encMode = 1;
        CHECK(processTrafficBurst(l.data(), l.data() + 216, scr, e2, &s2) == VoiceVerdict::EncryptedNotDecoded && s2.calls == 0, "encrypted evidence refused");
    }
    {   // state unknown: not decoded
        Sink s; CallEncryption e;
        CHECK(processTrafficBurst(l.data(), l.data() + 216, scr, e, &s) == VoiceVerdict::StateUnknownNotDecoded && s.calls == 0, "unknown state refused");
    }
    {   // the codec loader path: a silent fake codec stands in for the user-installed plug-in
        struct Silent final : dect2::codec::SpeechCodec {
            int sampleRate() const override { return 8000; }
            std::vector<int16_t> decode(const std::vector<uint8_t>& c) override { return std::vector<int16_t>(c.size() * 4, 0); }
        };
        auto setup = [](TetraVoiceSink& vs, size_t& pcmN, bool withCodec) {
            if (withCodec) vs.setCodec(std::make_unique<Silent>());
            // test-only stand-in for the (not yet implemented) traffic channel decoder: pack 8 bits per byte
            vs.setFrameDecoder([](const uint8_t* b, size_t n) { std::vector<uint8_t> o(n / 8); for (size_t i = 0; i < o.size(); i++) o[i] = b[i * 8]; return o; });
            vs.setPcmOut([&pcmN](const int16_t*, size_t n, int) { pcmN += n; });
        };
        {   TetraVoiceSink vs; size_t pcmN = 0; setup(vs, pcmN, true);
            CallEncryption e; e.known = true;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 54 * 4 && vs.framesDecoded() == 1, "clear burst gives PCM through the codec sink (%zu)", pcmN);
            e.encMode = 1;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 54 * 4 && vs.framesDecoded() == 1, "encrypted burst gives no further PCM");
        }
        {   TetraVoiceSink vs; size_t pcmN = 0; setup(vs, pcmN, false);
            CallEncryption e; e.known = true;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 0 && vs.status().find("not installed") != std::string::npos, "no codec: no audio, not-installed text");
        }
        {   TetraVoiceSink vs; vs.setCodec(std::make_unique<Silent>()); size_t pcmN = 0;
            vs.setPcmOut([&pcmN](const int16_t*, size_t n, int) { pcmN += n; });
            CallEncryption e; e.known = true;
            processTrafficBurst(l.data(), l.data() + 216, scr, e, &vs);
            CHECK(pcmN == 0, "no traffic channel decoder: nothing reaches the codec");
        }
    }
    {   // one voice stream: a group call heard on three sites in one capture reaches the codec once
        VoiceStreamPicker p; int tn = 0;
        p.newCapture();
        CHECK(p.offer(100, 10, tn) && tn == -1, "nothing followed: the first carrier may give voice");
        p.heard(100, 2, 10);
        CHECK(!p.offer(101, 10, tn) && !p.offer(102, 10, tn), "the other copies of the call in the same capture stay out");
        p.newCapture();
        CHECK(!p.offer(101, 13, tn), "next capture: another carrier stays out while the call is followed");
        CHECK(p.offer(100, 13, tn) && tn == 2, "next capture: the followed carrier, only its timeslot");
        p.heard(100, 2, 13);
        p.newCapture();
        CHECK(p.offer(100, 16, tn) && tn == 2, "still followed");
        p.heard(100, 0, 16);   // the call is over
        CHECK(p.offer(101, 16, tn) && tn == -1, "a quiet followed carrier frees the codec in the same capture");
        p.heard(101, 3, 16);
        p.newCapture();
        CHECK(!p.offer(100, 20, tn) && p.offer(101, 20, tn) && tn == 3, "the new call is followed");
        p.newCapture();
        CHECK(p.offer(100, 20 + VoiceStreamPicker::kHoldSec + 1, tn) && tn == -1, "a followed carrier not heard for kHoldSec is let go");
    }
    printf(fails ? "tetra voice: %d FAILED\n" : "tetra voice: all passed\n", fails);
    return fails ? 1 : 0;
}
