// Ham keyboard modes: text to audio for PSK31 / PSK63, RTTY, CW and the hfdig keyboard families, the transmit limits and the macros.
#include "dect2/ham/ham_kbd.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/psk/psk_varicode.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "../hfdig/hfdig_kbd_int.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2::ham {

namespace {
constexpr double kPi = 3.14159265358979323846;
std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
}

const std::vector<KbdPreset>& kbdPresets() {
    //                        id             name           kind              baud   shift  fam  tones bw    var  ADIF mode / submode          rx family, sub   psk  Hz
    static const std::vector<KbdPreset> v = {
        {"psk31",        "PSK31",        KbdKind::Psk,   31.25, 0,   0,   0,  0,    0, "PSK",      "PSK31",           36, nullptr,  0, 1000},
        {"psk63",        "PSK63",        KbdKind::Psk,   62.5,  0,   0,   0,  0,    0, "PSK",      "PSK63",           36, nullptr,  1, 1000},
        {"rtty",         "RTTY",         KbdKind::Rtty,  45.45, 170, 0,   0,  0,    0, "RTTY",     "",                25, "rtty",   0, 2210},
        {"cw",           "CW",           KbdKind::Cw,    0,     0,   0,   0,  0,    0, "CW",       "",                34, nullptr,  0, 700},
        {"olivia-8-250", "Olivia 8/250", KbdKind::Hfdig, 0,     0,   KbdOlivia,    8,  250,  0, "OLIVIA",   "OLIVIA 8/250",    25, "olivia", 0, 1500},
        {"olivia-16-500","Olivia 16/500",KbdKind::Hfdig, 0,     0,   KbdOlivia,    16, 500,  0, "OLIVIA",   "OLIVIA 16/500",   25, "olivia", 0, 1500},
        {"olivia-32-1000","Olivia 32/1000",KbdKind::Hfdig,0,    0,   KbdOlivia,    32, 1000, 0, "OLIVIA",   "OLIVIA 32/1000",  25, "olivia", 0, 1500},
        {"contestia-8-250","Contestia 8/250",KbdKind::Hfdig,0, 0,   KbdContestia,  8,  250,  0, "CONTESTI", "CONTESTI 8/250",  25, "contestia", 0, 1500},
        {"contestia-16-500","Contestia 16/500",KbdKind::Hfdig,0,0,  KbdContestia,  16, 500,  0, "CONTESTI", "CONTESTI 16/500", 25, "contestia", 0, 1500},
        {"contestia-32-1000","Contestia 32/1000",KbdKind::Hfdig,0,0,KbdContestia,  32, 1000, 0, "CONTESTI", "CONTESTI 32/1000",25, "contestia", 0, 1500},
        {"mfsk16",       "MFSK16",       KbdKind::Hfdig, 0,     0,   KbdMfsk,      0,  0,    3, "MFSK",     "MFSK16",          25, "mfsk",   0, 1500},
        {"mfsk32",       "MFSK32",       KbdKind::Hfdig, 0,     0,   KbdMfsk,      0,  0,    6, "MFSK",     "MFSK32",          25, "mfsk",   0, 1500},
        {"thor16",       "THOR 16",      KbdKind::Hfdig, 0,     0,   KbdThor,      0,  0,    4, "THOR",     "",                25, "thor",   0, 1500},
        {"thor22",       "THOR 22",      KbdKind::Hfdig, 0,     0,   KbdThor,      0,  0,    5, "THOR",     "",                25, "thor",   0, 1500},
        {"dominoex11",   "DominoEX 11",  KbdKind::Hfdig, 0,     0,   KbdDominoex,  0,  0,    3, "DOMINO",   "DOMINOEX",        25, "dominoex", 0, 1500},
        {"feldhell",     "Feld Hell",    KbdKind::Hfdig, 0,     0,   KbdHell,      0,  0,    0, "HELL",     "FELDHELL",        25, "hell",   0, 1500},
    };
    return v;
}
const KbdPreset* kbdPresetById(const std::string& id) {
    for (const auto& p : kbdPresets()) if (id == p.id) return &p;
    return nullptr;
}

static KbdConfig hfdigConfig(const KbdPreset& p, double centreHz) {
    KbdConfig k;
    k.family = p.family;
    k.tones = p.tones ? p.tones : 32;
    k.bandwidth = p.bw ? p.bw : 1000;
    k.variant = p.variant;
    k.centreHz = centreHz;
    k.autoCentre = false;
    return k;
}

double kbdBwHz(const KbdPreset& p) {
    switch (p.kind) {
    case KbdKind::Psk: return 2 * p.baud;
    case KbdKind::Rtty: return p.shiftHz + 1.5 * p.baud + 10;
    case KbdKind::Cw: return 200;
    default: return kbdBandwidthHz(hfdigConfig(p, 1500));
    }
}

double kbdSeconds(const std::vector<float>& a) { return (double)a.size() / kKbdRate; }

namespace {

// 10 ms fades at both ends (no key clicks), 0.15 s of silence before (PTT to audio) and 0.1 s after
void finish(std::vector<float>& a) {
    const size_t f = std::min<size_t>(a.size() / 2, kKbdRate / 100);
    for (size_t i = 0; i < f; i++) {
        const float g = 0.5f - 0.5f * (float)std::cos(kPi * (double)i / (double)f);
        a[i] *= g;
        a[a.size() - 1 - i] *= g;
    }
    std::vector<float> o((size_t)(0.15 * kKbdRate), 0.f);
    o.insert(o.end(), a.begin(), a.end());
    o.insert(o.end(), (size_t)(0.1 * kKbdRate), 0.f);
    a.swap(o);
}

// ---- PSK31 / PSK63: differential BPSK, a "0" is a phase reversal, the carrier amplitude follows half a cosine across the reversal ----
std::vector<float> renderPsk(const KbdPreset& p, const std::string& text, double hz) {
    std::string t;
    for (char c : text) if ((unsigned char)c < 128) t += c;
    std::vector<uint8_t> bits(32, 0);                       // preamble: idle reversals
    const auto d = psk::varicodeBits(t);
    bits.insert(bits.end(), d.begin(), d.end());
    bits.insert(bits.end(), 16, 0);                         // postamble
    const int N = (int)std::lround(kKbdRate / p.baud);
    std::vector<float> a;
    a.reserve(bits.size() * (size_t)N);
    double ph = 0;
    const double w = 2 * kPi * hz / kKbdRate;
    float prev = 1;
    for (uint8_t b : bits) {
        const float cur = b ? prev : -prev;
        for (int k = 0; k < N; k++) {
            const double sh = b ? 1.0 : 0.5 + 0.5 * std::cos(kPi * k / N);   // weight of the previous symbol across the interval
            a.push_back((float)((prev * sh + cur * (1 - sh)) * std::sin(ph)));
            ph += w;
            if (ph > 2 * kPi) ph -= 2 * kPi;
        }
        prev = cur;
    }
    return a;
}

// ---- RTTY: ITA2 Baudot, 5 data bits LSB first, 1 start bit, 1.5 stop bits, phase continuous FSK; mark is the lower tone ----
std::vector<float> renderRtty(const KbdPreset& p, const std::string& text, double centre) {
    static const char kL[] = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";   // letters; 27 is FIGS, 31 is LTRS
    static const char kF[] = {0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', '$', '4', 0, ',', '!', ':', '(', '5', '"', ')', '2', '#', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};
    std::vector<int> codes;
    bool figs = false;
    codes.push_back(31);
    for (char ch0 : upper(text)) {
        if (ch0 == '\n') { codes.push_back(8); codes.push_back(2); continue; }
        int code = -1;
        bool needFigs = false;
        for (int k = 0; k < 32 && code < 0; k++) if (kL[k] == ch0 && ch0 != 0 && k != 27 && k != 31) code = k;
        if (code < 0) { for (int k = 0; k < 32 && code < 0; k++) if (kF[k] == ch0 && ch0 != 0) code = k; needFigs = true; }
        if (code < 0) continue;
        if (code != 4 && code != 8 && code != 2 && needFigs != figs) { codes.push_back(needFigs ? 27 : 31); figs = needFigs; }
        codes.push_back(code);
    }
    codes.push_back(31);
    struct Seg { bool mark; double bits; };
    std::vector<Seg> segs;
    segs.push_back({true, 20});
    for (int c : codes) {
        segs.push_back({false, 1});
        for (int b = 0; b < 5; b++) segs.push_back({((c >> b) & 1) != 0, 1});
        segs.push_back({true, 1.5});
    }
    segs.push_back({true, 10});
    const double lo = centre - p.shiftHz / 2, hi = centre + p.shiftHz / 2;
    const double spb = kKbdRate / p.baud;
    std::vector<float> a;
    double ph = 0;
    for (const Seg& s : segs) {
        const size_t n = (size_t)std::llround(s.bits * spb);
        const double w = 2 * kPi * (s.mark ? lo : hi) / kKbdRate;
        for (size_t i = 0; i < n; i++) {
            a.push_back((float)std::sin(ph));
            ph += w;
            if (ph > 2 * kPi) ph -= 2 * kPi;
        }
    }
    return a;
}

// ---- CW: raised-cosine keyed tone (5 ms edges), the usual 1 : 3 : 7 timing, prosigns as <AR> ----
std::vector<float> renderCw(const std::string& text, double hz, double wpm) {
    wpm = std::max(5.0, std::min(60.0, wpm));
    const double dot = 1.2 / wpm;
    const size_t ed = (size_t)(0.005 * kKbdRate);
    std::vector<float> a;
    double ph = 0;
    const double w = 2 * kPi * hz / kKbdRate;
    auto tone = [&](double sec) {
        const size_t n = (size_t)std::llround(sec * kKbdRate);
        for (size_t i = 0; i < n; i++) {
            double g = 1;
            if (i < ed) g = 0.5 - 0.5 * std::cos(kPi * (double)i / (double)ed);
            else if (n - 1 - i < ed) g = 0.5 - 0.5 * std::cos(kPi * (double)(n - 1 - i) / (double)ed);
            a.push_back((float)(g * std::sin(ph)));
            ph += w;
            if (ph > 2 * kPi) ph -= 2 * kPi;
        }
    };
    auto gap = [&](double sec) { a.insert(a.end(), (size_t)std::llround(sec * kKbdRate), 0.f); };
    bool first = true;
    for (const std::string& tok : cw::tokenize(text)) {
        if (tok == " ") { gap(4 * dot); continue; }   // a word space is 7 dots: 3 after the character and these 4
        const std::string code = cw::encodeSymbol(tok);
        if (code.empty()) continue;
        if (!first) gap(0);
        first = false;
        for (size_t i = 0; i < code.size(); i++) {
            tone(code[i] == '-' ? 3 * dot : dot);
            gap(i + 1 < code.size() ? dot : 3 * dot);
        }
    }
    return a;
}

// ---- the keyboard families of hfdig: their modulators make 8 kHz audio; interpolated to 48 kHz ----
std::vector<float> renderHfdig(const KbdPreset& p, const std::string& text, double hz) {
    const KbdConfig k = hfdigConfig(p, hz);
    std::unique_ptr<kbd::Modulator> mod;
    switch (k.family) {
    case KbdOlivia: case KbdContestia: mod = kbd::makeOliviaMod(k, hz); break;
    case KbdMfsk: mod = kbd::makeMfskMod(k, hz); break;
    case KbdThor: case KbdDominoex: mod = kbd::makeIfkMod(k, hz); break;
    default: mod = kbd::makeHellMod(k, hz); break;
    }
    if (!mod) return {};
    kbd::TextSrc src;
    src.text = text + std::string(64, ' ');   // the padding fills the last block; the text never wraps
    const uint64_t want = text.size() + (k.family == KbdMfsk ? 40 : 16);   // MFSK: the interleaver and the Viterbi decoder hold the last characters back
    std::vector<float> raw((size_t)(0.1 * kbd::kFs), 0.f);
    while (src.sent < want && raw.size() < (size_t)(kKbdMaxBurstSec * 2 * kbd::kFs)) mod->next(raw, src);
    // 8 kHz -> 48 kHz: windowed sinc, 6 phases
    static std::vector<double> h;
    constexpr int N = 48;
    if (h.empty()) {
        for (int i = -N; i <= N; i++) {
            const double x = kPi * i * 2 * 3600.0 / kKbdRate;
            h.push_back((i == 0 ? 1.0 : std::sin(x) / x) * (0.5 + 0.5 * std::cos(kPi * i / (N + 1))) * 2 * 3600.0 / kKbdRate * 6);
        }
    }
    std::vector<float> o(raw.size() * 6, 0.f);
    for (size_t n = 0; n < o.size(); n++) {
        double acc = 0;
        for (int i = -N; i <= N; i++) {
            const long m = (long)n - i;
            if (m < 0 || m % 6) continue;
            const size_t j = (size_t)(m / 6);
            if (j < raw.size()) acc += h[(size_t)(i + N)] * raw[j];
        }
        o[n] = (float)acc;
    }
    return o;
}

}  // namespace

std::vector<float> kbdRender(const KbdPreset& p, const std::string& text, double centreHz, double cwWpm, std::string& err) {
    err.clear();
    if (text.empty()) { err = "nothing to send"; return {}; }
    if (text.size() > kKbdMaxChars) { err = "the text is too long for one transmission (" + std::to_string(kKbdMaxChars) + " characters at most)"; return {}; }
    std::vector<float> a;
    switch (p.kind) {
    case KbdKind::Psk: a = renderPsk(p, text, centreHz); break;
    case KbdKind::Rtty: a = renderRtty(p, text, centreHz); break;
    case KbdKind::Cw: a = renderCw(text, centreHz, cwWpm); break;
    default: a = renderHfdig(p, text, centreHz); break;
    }
    if (a.empty()) { err = "nothing in the text can be sent in this mode"; return {}; }
    float pk = 0;
    for (float v : a) pk = std::max(pk, std::fabs(v));
    if (pk <= 0) { err = "nothing in the text can be sent in this mode"; return {}; }
    for (auto& v : a) v /= pk;
    finish(a);
    if (kbdSeconds(a) > kKbdMaxBurstSec) { err = "the transmission would take " + std::to_string((int)kbdSeconds(a)) + " s: send less text at a time (" + std::to_string((int)kKbdMaxBurstSec) + " s at most)"; return {}; }
    return a;
}

// ---- limits ----
double TxDuty::used(double now) const {
    double u = 0;
    for (const auto& b : b_) {
        const double s = std::max(b.first, now - win_), e = std::min(b.first + b.second, now);
        if (e > s) u += e - s;
    }
    return u;
}
bool TxDuty::allow(double now, double dur) const { return used(now) + dur <= frac_ * win_; }
void TxDuty::add(double now, double dur) {
    b_.erase(std::remove_if(b_.begin(), b_.end(), [&](const std::pair<double, double>& x) { return x.first + x.second < now - win_; }), b_.end());
    b_.push_back({now, dur});
}

bool kbdSend(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gateIn, const KbdPreset& p, const std::string& text, double centreHz,
             double level, double cwWpm, double nowSec, std::string& err) {
    std::vector<float> a = kbdRender(p, text, centreHz, cwWpm, err);
    if (a.empty()) return false;
    const double dur = kbdSeconds(a);
    if (!duty.allow(nowSec, dur)) {
        char b[160];
        snprintf(b, sizeof b, "duty cycle limit: %.0f s on air in the last 10 minutes, wait before sending more", duty.used(nowSec));
        err = b;
        return false;
    }
    HamTxCheck g = gateIn;
    const double bw = kbdBwHz(p);
    g.audioHz = std::max(0.0, centreHz - bw / 2);   // the gate takes the lowest frequency of the signal
    g.bwHz = bw;
    if (!tx.sendAudio(g, std::move(a), bw, level, err)) return false;
    duty.add(nowSec, dur);
    return true;
}

// ---- macros ----
const std::vector<KbdMacro>& kbdDefaultMacros() {
    static const std::vector<KbdMacro> m = {
        {"CQ", "CQ CQ CQ DE {MYCALL} {MYCALL} {MYCALL} PSE K\n"},
        {"Answer", "{CALL} DE {MYCALL} {MYCALL} K\n"},
        {"RST", "{CALL} DE {MYCALL} UR RST {RST} {RST} K\n"},
        {"QTH", "MY QTH IS {MYQTH} GRID {MYGRID} K\n"},
        {"Name", "MY NAME IS {MYNAME} {MYNAME} K\n"},
        {"73", "{CALL} DE {MYCALL} TNX FER QSO 73 SK\n"},
    };
    return m;
}

bool kbdExpand(const std::string& t, const KbdStation& s, std::string& out, std::string* missing) {
    out.clear();
    bool ok = true;
    for (size_t i = 0; i < t.size();) {
        const size_t e = t[i] == '{' ? t.find('}', i) : std::string::npos;
        if (e == std::string::npos) { out += t[i++]; continue; }
        const std::string k = t.substr(i + 1, e - i - 1);
        const std::string* v = k == "MYCALL" ? &s.myCall : k == "MYGRID" ? &s.myGrid : k == "MYNAME" ? &s.myName : k == "MYQTH" ? &s.myQth
                             : k == "CALL" ? &s.call : k == "NAME" ? &s.name : k == "RST" ? &s.rst : nullptr;
        if (!v) { out += t.substr(i, e - i + 1); i = e + 1; continue; }
        if (v->empty()) { ok = false; if (missing && missing->find(k) == std::string::npos) *missing += (missing->empty() ? "" : ", ") + k; }
        out += upper(*v);
        i = e + 1;
    }
    return ok;
}

std::vector<std::string> kbdCallsInText(const std::string& text, const std::string& myCall) {
    std::vector<std::string> r;
    const std::string me = upper(myCall);
    for (const auto& c : cw::callsigns(upper(text))) if (c != me && hamCallValid(c)) r.push_back(c);
    return r;
}

QsoRecord kbdLogRecord(const KbdPreset& p, const KbdStation& s, double freqHz, double startUtc, double endUtc, const std::string& rstSent, const std::string& rstRcvd) {
    QsoRecord q;
    q.call = upper(s.call);
    q.mode = p.adifMode;
    q.submode = p.adifSub;
    q.rstSent = rstSent; q.rstRcvd = rstRcvd;
    q.myCall = upper(s.myCall); q.myGrid = upper(s.myGrid);
    q.freqHz = freqHz; q.startUtc = startUtc; q.endUtc = endUtc;
    return q;
}

}  // namespace dect2::ham
