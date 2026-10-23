// Hydrogen line toolkit: FITS writer / reader (own code, from the FITS 4.0 standard: 80-character cards, 2880-byte blocks, big-endian data;
// primary and IMAGE extension HDUs with BITPIX -32 / -64, and BINTABLE extensions with D / E columns), CSV, SigMF text, saved sessions.
#include "dect2/astro/hline.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace dect2 {
namespace astro {
namespace hl {

namespace {
typedef std::vector<std::pair<std::string, std::string>> Cards;

bool isNumberText(const std::string& s) {
    if (s.empty()) return false;
    char* e = nullptr;
    std::strtod(s.c_str(), &e);
    return e && *e == 0;
}
std::string card(const std::string& key, const std::string& val) {
    std::string v;
    if (isNumberText(val) || val == "T" || val == "F") { v = val; if (v.size() < 20) v = std::string(20 - v.size(), ' ') + v; }
    else {
        std::string q;
        for (char c : val) { q += c; if (c == '\'') q += '\''; }
        v = "'" + q + "'";
        if (v.size() < 10) v += std::string(10 - v.size(), ' ');
    }
    std::string k = key.substr(0, 8);
    k += std::string(8 - k.size(), ' ');
    std::string s = k + "= " + v;
    s.resize(80, ' ');
    return s.substr(0, 80);
}
std::string header(const Cards& c) {
    std::string h;
    for (const auto& kv : c) h += card(kv.first, kv.second);
    h += std::string("END").append(77, ' ');
    while (h.size() % 2880) h += ' ';
    return h;
}
std::string num(long long v) { return std::to_string(v); }
void putBE(std::string& o, double v, bool dbl) {
    if (dbl) { uint64_t u; std::memcpy(&u, &v, 8); for (int i = 7; i >= 0; i--) o += (char)((u >> (i * 8)) & 0xFF); }
    else { const float f = (float)v; uint32_t u; std::memcpy(&u, &f, 4); for (int i = 3; i >= 0; i--) o += (char)((u >> (i * 8)) & 0xFF); }
}
double getBE(const unsigned char* p, bool dbl) {
    if (dbl) { uint64_t u = 0; for (int i = 0; i < 8; i++) u = (u << 8) | p[i]; double v; std::memcpy(&v, &u, 8); return v; }
    uint32_t u = 0; for (int i = 0; i < 4; i++) u = (u << 8) | p[i];
    float f; std::memcpy(&f, &u, 4); return f;
}
void pad(std::string& s) { while (s.size() % 2880) s += '\0'; }
} // namespace

std::string fitsImage(const std::vector<float>& d, int nx, int ny, const Cards& extra, bool primary) {
    Cards c;
    if (primary) c.push_back({"SIMPLE", "T"}); else c.push_back({"XTENSION", "IMAGE"});
    c.push_back({"BITPIX", "-32"}); c.push_back({"NAXIS", "2"}); c.push_back({"NAXIS1", num(nx)}); c.push_back({"NAXIS2", num(ny)});
    if (!primary) { c.push_back({"PCOUNT", "0"}); c.push_back({"GCOUNT", "1"}); }
    c.insert(c.end(), extra.begin(), extra.end());
    std::string out = header(c), data;
    for (float v : d) putBE(data, v, false);
    pad(data);
    return out + data;
}

std::string fitsTable(const std::vector<std::string>& names, const std::vector<std::string>& units, const std::vector<std::vector<double>>& cols, const Cards& extra, bool dbl) {
    const size_t nc = cols.size(), nr = nc ? cols[0].size() : 0;
    Cards c;
    c.push_back({"XTENSION", "BINTABLE"}); c.push_back({"BITPIX", "8"}); c.push_back({"NAXIS", "2"});
    c.push_back({"NAXIS1", num((long long)nc * (dbl ? 8 : 4))}); c.push_back({"NAXIS2", num((long long)nr)});
    c.push_back({"PCOUNT", "0"}); c.push_back({"GCOUNT", "1"}); c.push_back({"TFIELDS", num((long long)nc)});
    for (size_t i = 0; i < nc; i++) {
        c.push_back({"TTYPE" + std::to_string(i + 1), i < names.size() ? names[i] : "COL"});
        c.push_back({"TFORM" + std::to_string(i + 1), dbl ? "1D" : "1E"});
        if (i < units.size() && !units[i].empty()) c.push_back({"TUNIT" + std::to_string(i + 1), units[i]});
    }
    c.insert(c.end(), extra.begin(), extra.end());
    std::string out = header(c), data;
    for (size_t r = 0; r < nr; r++) for (size_t i = 0; i < nc; i++) putBE(data, cols[i][r], dbl);
    pad(data);
    return out + data;
}

std::string fitsFile(const Cards& primary, const std::vector<std::string>& ext) {
    Cards c{{"SIMPLE", "T"}, {"BITPIX", "8"}, {"NAXIS", "0"}, {"EXTEND", "T"}};
    c.insert(c.end(), primary.begin(), primary.end());
    std::string o = header(c);
    for (const auto& e : ext) o += e;
    return o;
}

bool fitsRead(const std::string& b, std::vector<Hdu>& out) {
    out.clear();
    size_t pos = 0;
    while (pos + 2880 <= b.size()) {
        Hdu h;
        bool end = false;
        while (!end) {
            if (pos + 2880 > b.size()) return false;
            for (int i = 0; i < 36 && !end; i++) {
                const std::string cd = b.substr(pos + (size_t)i * 80, 80);
                std::string key = cd.substr(0, 8);
                while (!key.empty() && key.back() == ' ') key.pop_back();
                if (key == "END") { end = true; break; }
                if (cd.compare(8, 2, "= ") != 0) continue;
                std::string v = cd.substr(10);
                size_t a = v.find_first_not_of(' ');
                if (a == std::string::npos) continue;
                v = v.substr(a);
                if (v[0] == '\'') {
                    std::string s;
                    size_t k = 1;
                    for (; k < v.size(); k++) { if (v[k] == '\'') { if (k + 1 < v.size() && v[k + 1] == '\'') { s += '\''; k++; } else break; } else s += v[k]; }
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                    v = s;
                } else {
                    const size_t sl = v.find('/');
                    if (sl != std::string::npos) v = v.substr(0, sl);
                    while (!v.empty() && v.back() == ' ') v.pop_back();
                }
                h.cards[key] = v; h.cardOrder.push_back(key);
            }
            pos += 2880;
        }
        auto geti = [&](const char* k) { auto it = h.cards.find(k); return it == h.cards.end() ? 0LL : std::atoll(it->second.c_str()); };
        h.bitpix = (int)geti("BITPIX");
        const int na = (int)geti("NAXIS");
        for (int i = 1; i <= na; i++) h.naxis.push_back((int)geti(("NAXIS" + std::to_string(i)).c_str()));
        const bool table = h.cards.count("XTENSION") && h.cards["XTENSION"] == "BINTABLE";
        h.isTable = table;
        size_t bytes = 0;
        if (na > 0) { bytes = (size_t)(std::abs(h.bitpix) / 8); for (int n : h.naxis) bytes *= (size_t)n; bytes += (size_t)geti("PCOUNT"); }
        if (pos + bytes > b.size()) return false;
        const unsigned char* d = (const unsigned char*)b.data() + pos;
        if (table) {
            const int nf = (int)geti("TFIELDS"), rows = h.naxis.size() > 1 ? h.naxis[1] : 0, rb = h.naxis.empty() ? 0 : h.naxis[0];
            std::vector<int> off; int o = 0;
            for (int i = 1; i <= nf; i++) {
                const std::string f = h.cards["TFORM" + std::to_string(i)];
                size_t k = 0; while (k < f.size() && isdigit((unsigned char)f[k])) k++;
                const int rep = k ? std::atoi(f.substr(0, k).c_str()) : 1;
                const char t = k < f.size() ? f[k] : 0;
                if (rep != 1 || (t != 'D' && t != 'E')) return false;
                h.colType.push_back(t); off.push_back(o); o += t == 'D' ? 8 : 4;
                h.colNames.push_back(h.cards["TTYPE" + std::to_string(i)]); h.colUnits.push_back(h.cards.count("TUNIT" + std::to_string(i)) ? h.cards["TUNIT" + std::to_string(i)] : "");
            }
            if (o != rb) return false;
            h.cols.assign((size_t)nf, {});
            for (int r = 0; r < rows; r++) for (int i = 0; i < nf; i++) h.cols[(size_t)i].push_back(getBE(d + (size_t)r * rb + off[(size_t)i], h.colType[(size_t)i] == 'D'));
        } else if (na > 0) {
            if (h.bitpix != -32 && h.bitpix != -64) return false;
            const size_t cnt = bytes / (size_t)(std::abs(h.bitpix) / 8), w = (size_t)(std::abs(h.bitpix) / 8);
            for (size_t i = 0; i < cnt; i++) h.image.push_back(getBE(d + i * w, h.bitpix == -64));
        }
        pos += (bytes + 2879) / 2880 * 2880;
        out.push_back(std::move(h));
    }
    return !out.empty();
}

std::string spectrumCsv(const std::vector<double>& f, const std::vector<double>& vt, double lsr, const std::vector<double>& ta, const std::vector<bool>& flag, const std::string& comment) {
    std::string s;
    char b[200];
    std::istringstream is(comment);
    for (std::string l; std::getline(is, l);) s += "# " + l + "\n";
    snprintf(b, sizeof b, "# lsr_correction_kms,%.4f\n", lsr); s += b;
    s += "freq_hz,vel_topo_kms,vel_lsr_kms,ta_k,flagged\n";
    for (size_t i = 0; i < f.size(); i++) {
        snprintf(b, sizeof b, "%.3f,%.4f,%.4f,%.6g,%d\n", f[i], i < vt.size() ? vt[i] : 0.0, (i < vt.size() ? vt[i] : 0.0) + lsr, i < ta.size() ? ta[i] : 0.0, i < flag.size() && flag[i] ? 1 : 0);
        s += b;
    }
    return s;
}

static std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if (c == '\n') o += "\\n"; else if ((unsigned char)c >= 32) o += c; }
    return o;
}

std::string sigmfMeta(double fc, double rate, const std::string& desc, const std::vector<GaussComp>& fits, const std::string& iso, double lat, double lon, uint64_t n) {
    char b[400];
    std::string s = "{\n  \"global\": {\n    \"core:datatype\": \"cf32_le\",\n    \"core:version\": \"1.0.0\",\n";
    snprintf(b, sizeof b, "    \"core:sample_rate\": %.3f,\n", rate); s += b;
    s += "    \"core:description\": \"" + jsonEsc(desc) + "\",\n    \"core:recorder\": \"Eye of ZorCom hydrogen line toolkit\",\n";
    snprintf(b, sizeof b, "    \"core:geolocation\": {\"type\": \"Point\", \"coordinates\": [%.5f, %.5f]}\n  },\n", lon, lat); s += b;
    s += "  \"captures\": [\n    {\"core:sample_start\": 0, ";
    snprintf(b, sizeof b, "\"core:frequency\": %.3f, \"core:datetime\": \"%s\"}\n  ],\n", fc, jsonEsc(iso).c_str()); s += b;
    s += "  \"annotations\": [\n";
    for (size_t i = 0; i < fits.size(); i++) {
        const auto& g = fits[i];
        snprintf(b, sizeof b, "    {\"core:sample_start\": 0, \"core:sample_count\": %llu, \"core:label\": \"HI component %zu\", "
                 "\"onair:peak_k\": %.5g, \"onair:centre_kms\": %.4f, \"onair:centre_err_kms\": %.4f, \"onair:fwhm_kms\": %.4f, \"onair:area_k_kms\": %.5g}%s\n",
                 (unsigned long long)n, i + 1, g.amp, g.centre, g.dCentre, g.fwhm(), g.area(), i + 1 < fits.size() ? "," : "");
        s += b;
    }
    s += "  ]\n}\n";
    return s;
}

std::string sessionText(const Session& s) {
    std::string o = "#onair-hline-session 1\n";
    for (const auto& kv : s.info) o += kv.first + "=" + kv.second + "\n";
    char b[64];
    for (size_t i = 0; i < s.spectra.size(); i++) {
        snprintf(b, sizeof b, "spec %.3f %zu", i < s.times.size() ? s.times[i] : 0.0, s.spectra[i].size()); o += b;
        for (double v : s.spectra[i]) { snprintf(b, sizeof b, " %.9g", v); o += b; }
        o += "\n";
    }
    return o;
}

bool parseSession(const std::string& t, Session& s) {
    s = Session();
    std::istringstream is(t);
    std::string l;
    if (!std::getline(is, l) || l.compare(0, 20, "#onair-hline-session") != 0) return false;
    while (std::getline(is, l)) {
        if (l.compare(0, 5, "spec ") == 0) {
            std::istringstream ls(l.substr(5));
            double tm; size_t n;
            if (!(ls >> tm >> n)) return false;
            std::vector<double> v(n);
            for (size_t i = 0; i < n; i++) if (!(ls >> v[i])) return false;
            s.times.push_back(tm); s.spectra.push_back(std::move(v));
        } else {
            const size_t e = l.find('=');
            if (e != std::string::npos) s.info[l.substr(0, e)] = l.substr(e + 1);
        }
    }
    return true;
}

} // namespace hl
} // namespace astro
} // namespace dect2
