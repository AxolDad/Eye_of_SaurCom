// Eye of SaurCom - trunk_capture_check: run a recording of a real trunked system through our own decoders and print what they found.
// Copyright (C) the Eye of SaurCom authors. GPL-3.0-or-later.
//
// Input: raw 32-bit float mono samples (FM discriminator audio, as receivers and the SigIDWiki samples record it), at the given rate.
// Convert an audio file with any tool, e.g. ffmpeg -i in.mp3 -ac 1 -ar 48000 -f f32le out.f32.
//   trunk_capture_check mpt   file.f32 [rate]   MPT 1327 control or traffic channel (1200 bit/s FFSK)
//   trunk_capture_check ltr   file.f32 [rate]   LTR / Passport subaudible words
//   trunk_capture_check edacs file.f32 [rate]   EDACS control channel (9600 bit/s; add "nb" for 4800 bit/s)
//   trunk_capture_check p25p2 file.f32 [rate]   P25 Phase 2 (H-DQPSK seen through an FM discriminator): the S-ISCH, I-ISCH and DUIDs found
//   trunk_capture_check p2dib file.dib          the same on a stream of dibits (one per byte)
// The audio of the analog modes is turned back into FM (3 kHz per unit of the normalised audio) for the IQ receivers. Both polarities
// are tried. Nothing here is used by the program itself.
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/ltr/ltr_tel.h"
#include "dect2/mpt1327/mpt1327_gen.h"
#include "dect2/mpt1327/mpt1327_proto.h"
#include "dect2/mpt1327/mpt1327_rx.h"
#include "dect2/p25/p25p2.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

using namespace dect2;

static std::vector<float> readF32(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<float> v;
    if (!f) return v;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0);
    v.resize((size_t)(n / 4));
    f.read(reinterpret_cast<char*>(v.data()), (std::streamsize)(v.size() * 4));
    for (float& x : v) if (!std::isfinite(x)) x = 0;
    return v;
}

// audio at rate -> IQ at outRate: normalised, resampled linearly, FM at devHz per unit
static std::vector<cf32> toIq(const std::vector<float>& a, double rate, double outRate, double devHz, bool invert) {
    float peak = 1e-9f;
    for (float x : a) peak = std::max(peak, std::fabs(x));
    std::vector<float> up((size_t)((double)a.size() * outRate / rate));
    for (size_t i = 0; i < up.size(); i++) {
        const double p = (double)i * rate / outRate;
        const size_t k = (size_t)p;
        const double fr = p - (double)k;
        const float x0 = a[std::min(k, a.size() - 1)], x1 = a[std::min(k + 1, a.size() - 1)];
        up[i] = (float)((x0 + (x1 - x0) * fr) / peak) * (invert ? -1.f : 1.f);
    }
    return mpt1327::fmModulate(up, outRate, devHz, 0.0);
}

static int runMpt(const std::vector<float>& a, double rate) {
    int total = 0;
    for (int inv = 0; inv < 2; inv++) {
        std::vector<float> x = a;
        if (inv) for (float& v : x) v = -v;
        mpt1327::Decoder d;
        for (size_t i = 0; i < x.size(); i += 4800) d.process(x.data() + i, std::min<size_t>(4800, x.size() - i), rate);
        const auto ev = d.takeEvents();
        const mpt1327::Stats& st = d.stats();
        std::printf("MPT 1327%s: codewords good %llu bad %llu, CCSC %llu (system %d), messages %llu, SYNT %llu, French %d\n", inv ? " (audio inverted)" : "",
                    (unsigned long long)st.good, (unsigned long long)st.bad, (unsigned long long)st.ccsc, st.sys, (unsigned long long)st.messages,
                    (unsigned long long)st.synt, (int)st.french);
        std::map<std::string, int> types;
        for (const auto& e : ev) types[(e.traffic ? "traffic " : "") + e.msg.type]++;
        std::printf("  types:");
        for (const auto& kv : types) std::printf(" %s %d", kv.first.c_str(), kv.second);
        std::printf("\n");
        size_t shown = 0;
        for (const auto& e : ev) {
            if (e.msg.type == "ALH" || e.msg.type == "ALHS" || e.msg.type == "ALHD") continue;   // the Aloha flood: shown in the counts
            if (shown++ >= 14) break;
            std::printf("  %7.2f s %016llX %s%-6s %s -> %s  %s\n", e.t, (unsigned long long)e.msg.cw, e.traffic ? "[traffic] " : "", e.msg.type.c_str(),
                        mpt1327::addressText(e.msg.caller).c_str(), mpt1327::addressText(e.msg.called).c_str(), e.msg.detail.c_str());
        }
        total += (int)st.good;
    }
    return total > 0 ? 0 : 2;
}

static int runLtr(const std::vector<float>& a, double rate) {
    int total = 0;
    for (int inv = 0; inv < 2; inv++) {
        const auto iq = toIq(a, rate, 240000, 3000, inv == 1);
        LtrReceiver rx;
        rx.configure(240000);
        rx.setSignalOffset(0);
        rx.setSilent(true);
        for (size_t i = 0; i < iq.size(); i += 4096) rx.feed(iq.data() + i, std::min<size_t>(4096, iq.size() - i));
        LtrTelemetry t;
        rx.telemetry(t, 0);
        std::printf("LTR%s: sync patterns %llu, good words %llu, bad %llu, %s, data swing %.0f Hz\n", inv ? " (audio inverted)" : "", (unsigned long long)t.syncs,
                    (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.passport ? "Passport" : "LTR", t.deviationHz);
        size_t shown = 0;
        for (auto it = t.messages.rbegin(); it != t.messages.rend() && shown < 12; ++it, shown++)
            std::printf("  %7.2f s %s go-to/LCN %d home %d free %d check %02X %s%s\n", it->timeSec, it->talkgroup.c_str(), it->passport ? it->lcn : it->gotoCh,
                        it->home, it->freeCh, it->chk, it->kind.c_str(), it->inbound ? " (inbound)" : "");
        total += (int)t.blocksOk;
    }
    return total > 0 ? 0 : 2;
}

static int runEdacs(const std::vector<float>& a, double rate, bool nb) {
    int total = 0;
    for (int mode = 0; mode < 2; mode++)
        for (int inv = 0; inv < 2; inv++) {
            const auto iq = toIq(a, rate, 480000, nb ? 2500 : 3500, inv == 1);
            EdacsReceiver rx;
            rx.configure(480000);
            rx.setSignalOffset(0);
            rx.setNarrow(nb);
            rx.setSilent(true);
            for (size_t i = 0; i < iq.size(); i += 4096) rx.feed(iq.data() + i, std::min<size_t>(4096, iq.size() - i));
            EdacsTelemetry t;
            rx.telemetry(t, 0);
            const edacs::Mode md = mode ? edacs::Mode::Extended : edacs::Mode::Standard;
            std::printf("EDACS%s%s, %s set: syncs %llu, messages BCH good %llu, corrected %llu, refused %llu, frames %zu, end-of-transmission %llu\n", nb ? " NB" : "",
                        inv ? " (audio inverted)" : "", mode ? "EA" : "standard", (unsigned long long)t.syncs, (unsigned long long)t.bchOk,
                        (unsigned long long)t.bchFixed, (unsigned long long)t.msgBad, t.msgs.size(), (unsigned long long)t.eot);
            size_t shown = 0;
            for (const EdacsMsgRec& m : t.msgs) {
                if (shown++ >= 10) break;
                std::printf("  %7.2f s %07X %07X  %s\n", m.timeSec, m.m1, m.m2, edacs::describe(edacs::parse(m.m1, m.m2, md), md).c_str());
            }
            total += (int)(t.bchOk + t.bchFixed);
        }
    return total > 0 ? 0 : 2;
}

// Phase 2 on a dibit stream: S-ISCH words (at most 2 dibits wrong); the I-ISCH codeword of the frame after an S-ISCH pair (slots 2, 3 are
// followed by slot 4's I-ISCH 180 dibits after the second); the DUIDs of the bursts; and the framer itself.
struct P2Count { int sisch = 0, isch = 0, ischSlotOk = 0, duidOk = 0, duids[16] = {}; uint64_t sf = 0, bursts = 0, macOk = 0, macBad = 0; };
static P2Count countP2(const std::vector<uint8_t>& dib) {
    P2Count c;
    p25p2::Framer fr;
    for (uint8_t d : dib) {
        fr.push(d);
        for (const p25p2::Event& e : fr.take())
            if (e.kind == p25p2::Event::Mac && e.macOk)
                std::printf("  MAC %s on channel %d (timeslot %d): source %d, %s %d, service options %02X, algorithm %02X\n", p25p2::macName(e.mac.opcode), e.chan,
                            e.slot, e.mac.src, e.mac.group ? "group" : "unit", e.mac.tg, e.mac.svcOpts, e.mac.algId);
    }
    c.sf = fr.superframes(); c.bursts = fr.bursts(); c.macOk = fr.macOk(); c.macBad = fr.macBad();
    std::vector<size_t> hits;
    for (size_t i = 0; i + 20 <= dib.size(); i++) {
        int diff = 0;
        for (int k = 0; k < 20 && diff <= 2; k++) diff += dib[i + (size_t)k] != (int)((p25p2::kSisch >> (38 - 2 * k)) & 3);
        if (diff > 2) continue;
        hits.push_back(i);
        c.sisch++;
        if (i + 20 + 160 <= dib.size()) {   // the burst after this S-ISCH
            const uint8_t* b = &dib[i + 20];
            const uint8_t v = (uint8_t)(b[p25p2::kDuidPos[0]] << 6 | b[p25p2::kDuidPos[1]] << 4 | b[p25p2::kDuidPos[2]] << 2 | b[p25p2::kDuidPos[3]]);
            const int du = p25p2::duidDecode(v);
            if (du >= 0) { c.duidOk++; c.duids[du & 15]++; }
        }
        i += 19;
    }
    for (size_t k = 1; k < hits.size(); k++) {
        if (hits[k] - hits[k - 1] != 180) continue;   // the second S-ISCH of a pair: slot 3; slot 4's I-ISCH follows
        const size_t at = hits[k] + 180;
        if (at + 20 > dib.size()) continue;
        uint64_t cw = 0;
        for (int j = 0; j < 20; j++) cw = cw << 2 | dib[at + (size_t)j];
        const int info = p25p2::ischDecode(cw);
        if (info >= 0) { c.isch++; if (p25p2::ischSlot(info) % 4 == 0) c.ischSlotOk++; }   // slot 4 or 8 or 0: a multiple of 4
    }
    return c;
}
static void printP2(const char* what, const P2Count& c, size_t n) {
    std::printf("P25 Phase 2%s: %zu dibits, S-ISCH %d, I-ISCH decoded after an S-ISCH pair %d (slot 0 / 4 / 8 as expected: %d), DUIDs %d, framer superframes %llu bursts %llu MAC good %llu bad %llu\n",
                what, n, c.sisch, c.isch, c.ischSlotOk, c.duidOk, (unsigned long long)c.sf, (unsigned long long)c.bursts, (unsigned long long)c.macOk, (unsigned long long)c.macBad);
    if (c.duidOk) {
        std::printf("  DUIDs:");
        for (int k = 0; k < 16; k++) if (c.duids[k]) std::printf(" %s %d", p25p2::duidName(k), c.duids[k]);
        std::printf("\n");
    }
}

// The discriminator output of a phase-modulated signal is the derivative of its phase: FM with the right scale gives the phase back. The
// scale is not known (the recorder's audio gain), so a range is tried.
static int runP25p2(const std::vector<float>& a, double rate) {
    int best = 0;
    for (int inv = 0; inv < 2; inv++)
        for (double dev : {3000.0, 6000.0, 9500.0, 11000.0, 12500.0, 14000.0}) {
            const auto iq = toIq(a, rate, 96000, dev, inv == 1);
            p25p2::Demod dm;
            dm.configure(96000, 0);
            std::vector<uint8_t> dib;
            for (size_t i = 0; i < iq.size(); i += 4800) dm.feed(iq.data() + i, std::min<size_t>(4800, iq.size() - i), dib);
            const P2Count c = countP2(dib);
            char w[64];
            snprintf(w, sizeof w, "%s, %.0f Hz per unit", inv ? " (audio inverted)" : "", dev);
            printP2(w, c, dib.size());
            best = std::max(best, c.sisch);
        }
    return best > 0 ? 0 : 2;
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: trunk_capture_check mpt|ltr|edacs file.f32 [rate] [nb]\n"); return 1; }
    const std::string what = argv[1];
    if (what == "p2dib") {   // a file of dibits (one per byte, 0..3), e.g. from a symbol slicer of one's own
        std::ifstream f(argv[2], std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        for (auto& x : d) x &= 3;
        if (argc > 3 && std::string(argv[3]) == "bursts") {   // every unscrambled burst whose MAC passes RS and CRC, as 160 dibits in hex
            static const uint8_t zero[320] = {};
            for (size_t i = 0; i + 180 <= d.size(); i++) {
                p25p2::Burst b;
                p25p2::burstParse(&d[i + 20], zero, b);
                if (!b.macOk) continue;
                std::printf("burst at %zu, DUID %s, MAC %s: ", i, p25p2::duidName(b.duid), p25p2::macName(b.mac.opcode));
                for (int k = 0; k < 160; k += 2) std::printf("%X", d[i + 20 + (size_t)k] << 2 | d[i + 21 + (size_t)k]);
                std::printf("\n");
                i += 179;
            }
        }
        const P2Count c = countP2(d);
        printP2(" (dibits)", c, d.size());
        return c.sisch > 0 ? 0 : 2;
    }
    const double rate = argc > 3 ? std::atof(argv[3]) : 48000.0;
    const bool nb = argc > 4 && std::strcmp(argv[4], "nb") == 0;
    const std::vector<float> a = readF32(argv[2]);
    if (a.empty() || rate < 8000) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
    std::printf("%s: %.1f s at %.0f Hz\n", argv[2], (double)a.size() / rate, rate);
    if (what == "mpt") return runMpt(a, rate);
    if (what == "ltr") return runLtr(a, rate);
    if (what == "edacs") return runEdacs(a, rate, nb);
    if (what == "p25p2") return runP25p2(a, rate);
    std::fprintf(stderr, "unknown system %s\n", what.c_str());
    return 1;
}
