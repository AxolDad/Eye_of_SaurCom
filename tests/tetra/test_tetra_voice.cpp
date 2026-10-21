// TETRA voice path: an unencrypted generated traffic burst reaches the speech frame sink as the right coded frame; an
// encrypted one, and one with unknown encryption state, produce "not decoded" and the sink is never called.
#include "dect2/tetra/tetra_gen.h"
#include "dect2/tetra/tetra_phy.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_voice.h"
#include "dect2/tetra/tetra_voice_codec.h"
#include "dect2/source/source.h"
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
    {   // decodeCarrier: the timeslot filter, and the encryption gate in front of it (the clear cell of scene 1)
        const double rate = 300e3;
        dect2::SynthConfig sc; sc.mode = 48; sc.snrDb = 30; sc.modeOpt[0] = 1;
        auto syn = dect2::makeTetraSynth(sc, rate);
        std::vector<dect2::cf32> x((size_t)(rate * 2.3));
        syn->generate(x.data(), x.size());
        const dect2::TetraCellCfg cell = dect2::tetraTestCell(1, 1);
        const std::vector<dect2::cf32> s = channelize(x.data(), x.size(), rate, 392.6e6 - dect2::kTetraTestCentreHz);
        const int ts = cell.trafficTs, other = ts == 1 ? 2 : 1;
        const double now = 100;
        // a call set-up for the traffic timeslot of the cell's main carrier, as decodeCarrier would read it; enc: 0 clear, 1 encrypted, -2 none heard
        auto decode = [&](int enc, int voiceTn, Sink& sink) {
            CallTracker calls;
            calls.setMainCarrier(cell.si.mainCarrier);
            if (enc >= -1) { CallEvent e; e.kind = 1; e.callId = 7; e.enc = enc; e.slots = 1 << (4 - ts); e.carrier = cell.si.mainCarrier; calls.onEvent(e, now); }
            return decodeCarrier(s.data(), s.size(), -1, -1, -1, &sink, &calls, now, voiceTn);
        };
        {   Sink k; const CarrierDecode d = decode(0, -1, k);
            CHECK(d.trafficSlots > 0 && k.calls == d.voicePassed && k.calls > 0 && d.voiceTn == ts, "first clear timeslot: %d of %d traffic bursts to the sink, slot %d", k.calls, d.trafficSlots, d.voiceTn); }
        {   Sink k; const CarrierDecode d = decode(0, ts, k);
            CHECK(k.calls > 0 && d.voiceTn == ts, "the followed timeslot reaches the sink (%d)", k.calls); }
        {   Sink k; const CarrierDecode d = decode(0, other, k);
            CHECK(k.calls == 0 && d.voiceTn == 0 && d.voicePassed > 0, "another timeslot: clear but kept from the sink (%d passed)", d.voicePassed); }
        {   Sink k; const CarrierDecode d = decode(1, -1, k);
            CHECK(k.calls == 0 && d.voiceTn == 0 && d.voicePassed == 0 && d.voiceNotDecoded > 0, "encrypted call: nothing reaches the sink"); }
        {   Sink k; const CarrierDecode d = decode(-2, ts, k);
            CHECK(k.calls == 0 && d.voiceTn == 0 && d.voicePassed == 0, "no call set-up heard: nothing reaches the sink"); }
    }
    {   // decodeCarrier: a capture with no MAC-RESOURCE at all (frames 2 to 18 of the clear cell: null PDUs, SYSINFO and the traffic slot, as a
        // capture taken during a talk spurt looks on the air) still gives the voice of a call whose clear set-up was read from an earlier capture;
        // an encrypted PDU of the capture still refuses it, and so does a set-up never heard
        const dect2::TetraCellCfg cell = dect2::tetraTestCell(1, 1);
        const int ts = cell.trafficTs;
        const double now = 100;
        auto capture = [&](bool encryptedSignalling, bool setupHeard, Sink& sink, int& resources) {
            dect2::TetraCellCfg c = cell; c.encryptedSignalling = encryptedSignalling;
            Modulator mod; std::vector<dect2::cf32> x;
            const uint32_t a0 = 33u * 72u + 4u;   // multiframe 34, frame 2, timeslot 1: frame 1 (the only MAC-RESOURCE of the cell) is left out
            for (uint32_t a = 0; a < 17u * 4u; a++) mod.push(cellSlot(c, a0 + a), x);
            CallTracker calls; calls.setMainCarrier(c.si.mainCarrier);
            if (setupHeard) { CallEvent e; e.kind = 1; e.callId = 7; e.enc = 0; e.slots = 1 << (4 - ts); e.carrier = c.si.mainCarrier; calls.onEvent(e, now - 5); }
            const CarrierDecode d = decodeCarrier(x.data(), x.size(), -1, -1, -1, &sink, &calls, now, -1);
            resources = 0; for (const auto& m : d.macs) resources += m.resources;
            return d;
        };
        {   Sink k; int res = 0; const CarrierDecode d = capture(false, true, k, res);
            CHECK(d.synced && d.trafficSlots > 0 && res == 0, "the capture has traffic (%d) and no MAC-RESOURCE (%d)", d.trafficSlots, res);
            CHECK(k.calls > 0 && k.calls == d.voicePassed && d.voiceTn == ts, "no MAC-RESOURCE in the capture, call known clear: voice (%d of %d, slot %d, %s)", k.calls, d.trafficSlots, d.voiceTn, d.voiceStatus); }
        {   Sink k; int res = 0; const CarrierDecode d = capture(true, true, k, res);
            CHECK(res > 0 && k.calls == 0 && d.voicePassed == 0 && d.voiceNotDecoded > 0, "an encrypted PDU in the capture: nothing reaches the sink (%s)", d.voiceStatus); }
        {   Sink k; int res = 0; const CarrierDecode d = capture(false, false, k, res);
            CHECK(k.calls == 0 && d.voicePassed == 0 && d.voiceNotDecoded > 0, "no set-up heard: nothing reaches the sink"); }
    }
    printf(fails ? "tetra voice: %d FAILED\n" : "tetra voice: all passed\n", fails);
    return fails ? 1 : 0;
}
