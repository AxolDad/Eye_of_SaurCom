// Flipper Zero Sub-GHz files: see ism_subfile.h
#include "dect2/ism/ism_subfile.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}

bool contains(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

// The timings of a RAW file in order, folded into pulses: a run of positive values is one mark, the negative values after it its gap
struct Folder {
    IsmTrain& t;
    double mark = 0, gap = 0;
    bool open = false;      // a mark has started
    explicit Folder(IsmTrain& tr) : t(tr) {}
    void add(long v) {
        if (v == 0) return;
        if (v > 0) {
            if (open && gap > 0) { t.p.push_back({(float)mark, (float)gap}); mark = 0; gap = 0; }
            mark += (double)v; open = true;
        } else if (open) gap += (double)-v;   // a gap before the first mark is silence before the capture
    }
    void finish() {
        if (!open) return;
        t.p.push_back({(float)mark, (float)(gap > 0 ? gap : 50000.0)});   // the last gap: long, as the receiver ends a burst
        open = false; mark = 0; gap = 0;
    }
};

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool parseSubFile(const std::string& text, SubFile& out, std::string& err) {
    SubFile f;
    bool anyHeader = false, haveFreq = false, binBlock = false;
    int binBits = 0;                 // Bit_RAW of the block being read
    long binTotalBits = 0;
    std::vector<uint8_t> binBytes;   // Data_RAW of the block
    Folder fold(f.train);
    // BinRAW: the bits of a block become marks and gaps of TE each, appended to the train as they come
    auto flushBin = [&] {
        if (!binBlock) return;
        binBlock = false;
        const long n = binBits > 0 ? std::min<long>(binBits, (long)binBytes.size() * 8) : (long)binBytes.size() * 8;
        for (long i = 0; i < n; i++) {
            const bool one = (binBytes[(size_t)i / 8] >> (7 - (i % 8))) & 1;
            fold.add(one ? (long)f.teUs : -(long)f.teUs);
        }
        binTotalBits += n;
        binBits = 0; binBytes.clear();
    };
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        const std::string line = trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t c = line.find(':');
        if (c == std::string::npos) continue;
        const std::string key = trim(line.substr(0, c)), val = trim(line.substr(c + 1));
        if (key == "Filetype") {
            if (!contains(val, "SubGhz")) { err = "not a Sub-GHz file (Filetype: " + val + ")"; return false; }
            anyHeader = true;
            if (contains(val, "Key")) f.kind = SubFile::Key;
        } else if (key == "Version") f.version = atoi(val.c_str());
        else if (key == "Frequency") { f.freqHz = atof(val.c_str()); haveFreq = f.freqHz > 0; }
        else if (key == "Preset") {
            f.preset = val;
            f.fsk = contains(val, "FSK") || contains(val, "MSK");
        } else if (key == "Custom_preset_module") f.customModule = val;
        else if (key == "Custom_preset_data") f.customData = val;
        else if (key == "Protocol") {
            f.protocol = val;
            if (val == "RAW") f.kind = SubFile::Raw;
            else if (val == "BinRAW") f.kind = SubFile::BinRaw;
            else f.kind = SubFile::Key;
        } else if (key == "RAW_Data") {
            const char* p = val.c_str();
            while (*p) {
                char* q = nullptr;
                const long v = strtol(p, &q, 10);
                if (q == p) { p++; continue; }
                fold.add(v);
                f.values++;
                p = q;
            }
        } else if (key == "Bit") f.bits = atoi(val.c_str());
        else if (key == "TE") f.teUs = atoi(val.c_str());
        else if (key == "Key") f.key = val;
        else if (key == "Bit_RAW") { flushBin(); binBlock = true; binBits = atoi(val.c_str()); }
        else if (key == "Data_RAW") {
            if (!binBlock) binBlock = true;
            int hi = -1;
            for (char ch : val) {
                const int n = hexNibble(ch);
                if (n < 0) { hi = -1; continue; }
                if (hi < 0) hi = n; else { binBytes.push_back((uint8_t)(hi * 16 + n)); hi = -1; }
            }
        }
    }
    if (!anyHeader) { err = "not a Flipper Sub-GHz file (no Filetype line)"; return false; }
    if (!haveFreq) { err = "the file has no Frequency"; return false; }
    if (f.kind == SubFile::BinRaw) {
        if (f.teUs <= 0) { err = "a BinRAW file needs TE"; return false; }
        flushBin();
        fold.finish();
        f.values = (size_t)binTotalBits;
        if (f.train.p.empty()) { err = "the BinRAW file has no bits"; return false; }
    } else if (f.kind == SubFile::Raw) {
        fold.finish();
        if (f.train.p.empty()) { err = "the file has no RAW_Data timings"; return false; }
    } else if (f.protocol.empty()) { err = "the key file names no Protocol"; return false; }
    f.train.fsk = f.fsk;
    out = f;
    return true;
}

std::string subFileText(const IsmTrain& t, double freqHz) {
    std::string s = "Filetype: Flipper SubGhz RAW File\nVersion: 1\n";
    char b[96];
    snprintf(b, sizeof b, "Frequency: %.0f\n", freqHz);
    s += b;
    s += t.fsk ? "Preset: FuriHalSubGhzPreset2FSKDev476Async\n" : "Preset: FuriHalSubGhzPresetOok650Async\n";
    s += "Protocol: RAW\n";
    int onLine = 0;
    auto put = [&](long v) {
        if (onLine == 0) s += "RAW_Data:";
        snprintf(b, sizeof b, " %ld", v);
        s += b;
        if (++onLine == 512) { s += "\n"; onLine = 0; }
    };
    for (const auto& p : t.p) {
        const long m = std::max(1L, (long)(p.pulseUs + 0.5f)), g = std::max(1L, (long)(p.gapUs + 0.5f));
        put(m);
        put(-g);
    }
    if (onLine) s += "\n";
    return s;
}

} // namespace dect2
