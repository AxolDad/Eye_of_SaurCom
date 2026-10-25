#include "dect2/notam/notam.h"
#include "dect2/automation/json_mini.h"
#include "dect2/common/xmlmini.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace dect2::notam {

namespace json = dect2::automation::json;

namespace {

bool fail(std::string* err, const char* what) {
    if (err) *err = what;
    return false;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

std::string upper(std::string s) {
    for (char& c : s) c = (char)toupper((unsigned char)c);
    return s;
}

// "06/2203" and "6/2203" are one number: "6/2203". "" when it is not a number of that form.
std::string normNumber(const std::string& s) {
    const size_t slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size() || slash > 4) return {};
    for (size_t i = 0; i < s.size(); i++)
        if (i != slash && !isdigit((unsigned char)s[i])) return {};
    const long y = strtol(s.substr(0, slash).c_str(), nullptr, 10), n = strtol(s.substr(slash + 1).c_str(), nullptr, 10);
    if (s.size() - slash - 1 > 5) return {};
    return std::to_string(y) + "/" + std::to_string(n);
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

int64_t utcOf(int y, int mo, int d, int h, int mi, int s) {
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 24 || mi < 0 || mi > 59 || s < 0 || s > 60) return 0;
    return daysFromCivil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + s;
}

// The zones tfr.faa.gov writes local times in, as hours from UTC.
bool zoneOffset(const std::string& z, int& hours) {
    static const struct { const char* n; int h; } kZones[] = {
        {"UTC", 0}, {"GMT", 0}, {"Z", 0}, {"EST", -5}, {"EDT", -4}, {"CST", -6}, {"CDT", -5}, {"MST", -7}, {"MDT", -6},
        {"PST", -8}, {"PDT", -7}, {"AKST", -9}, {"AKDT", -8}, {"HST", -10}, {"AST", -4}, {"CHST", 10}, {"GUAM", 10}, {"SST", -11},
    };
    const std::string u = upper(trim(z));
    for (const auto& e : kZones)
        if (u == e.n) { hours = e.h; return true; }
    return false;
}

double haversineNm(double lat1, double lon1, double lat2, double lon2) {
    const double k = M_PI / 180;
    const double dl = (lat2 - lat1) * k, dn = (lon2 - lon1) * k;
    const double a = sin(dl / 2) * sin(dl / 2) + cos(lat1 * k) * cos(lat2 * k) * sin(dn / 2) * sin(dn / 2);
    return 2 * 3440.065 * asin(std::min(1.0, sqrt(a)));
}

// Where an item is, for deciding whether a regional source speaks for it.
bool positionOf(const Notam& n, double& lat, double& lon) {
    for (const Area& a : n.areas) {
        if (!a.ring.empty()) {
            lat = lon = 0;
            for (const auto& p : a.ring) { lat += p.first; lon += p.second; }
            lat /= (double)a.ring.size();
            lon /= (double)a.ring.size();
            return true;
        }
        if (a.radiusNm > 0) { lat = a.centreLat; lon = a.centreLon; return true; }
    }
    return false;
}

// An altitude limit as the FAA API writes it: "SFC", "GND", "1500FT", "1500FT AGL", "FL180", "UNL".
bool readLimit(const std::string& s0, int& ft, bool& agl) {
    const std::string s = upper(trim(s0));
    if (s.empty()) return false;
    agl = s.find("AGL") != std::string::npos;
    if (s.rfind("SFC", 0) == 0 || s.rfind("GND", 0) == 0) { ft = 0; agl = true; return true; }
    if (s.rfind("UNL", 0) == 0) { ft = 99999; return true; }
    if (s.rfind("FL", 0) == 0) { ft = atoi(s.c_str() + 2) * 100; agl = false; return ft > 0; }
    if (!isdigit((unsigned char)s[0])) return false;
    ft = atoi(s.c_str());
    return true;
}

}  // namespace

const char* sourceName(Source s) {
    switch (s) {
    case Source::FaaApi: return "FAA NOTAM API";
    case Source::TfrSite: return "tfr.faa.gov";
    case Source::Fisb: return "FIS-B";
    }
    return "?";
}

// ---------------------------------------------------------------- helpers

int64_t parseIsoUtc(const std::string& s0) {
    const std::string s = trim(s0);
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (s.size() < 10 || sscanf(s.c_str(), "%4d-%2d-%2d", &y, &mo, &d) != 3) return 0;
    size_t i = 10;
    if (i < s.size() && (s[i] == 'T' || s[i] == ' ')) {
        if (sscanf(s.c_str() + i + 1, "%2d:%2d", &h, &mi) != 2) return 0;
        i += 6;
        if (i < s.size() && s[i] == ':') { sec = atoi(s.c_str() + i + 1); i += 3; }
        while (i < s.size() && (s[i] == '.' || isdigit((unsigned char)s[i]))) i++;
    }
    int64_t t = utcOf(y, mo, d, h, mi, sec);
    if (!t) return 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
        int oh = 0, om = 0;
        if (sscanf(s.c_str() + i + 1, "%2d:%2d", &oh, &om) < 1) return 0;
        const int64_t off = (oh * 3600 + om * 60) * (s[i] == '+' ? 1 : -1);
        t -= off;
    }
    return t;
}

int64_t parseNotamTime(const std::string& s0) {
    const std::string s = trim(s0);
    if (s.size() < 10) return 0;
    for (int i = 0; i < 10; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    auto two = [&](int i) { return (s[i] - '0') * 10 + (s[i + 1] - '0'); };
    return utcOf(2000 + two(0), two(2), two(4), two(6), two(8), 0);
}

double distanceNm(const Notam& n, double lat, double lon) {
    double best = -1;
    for (const Area& a : n.areas) {
        double d;
        if (!a.ring.empty()) {
            // Inside by the crossing count; outside, the nearest vertex is close enough for "how near is it" (rings are densely drawn).
            bool in = false;
            const size_t m = a.ring.size();
            for (size_t i = 0, j = m - 1; i < m; j = i++) {
                const auto& pi = a.ring[i];
                const auto& pj = a.ring[j];
                if ((pi.first > lat) != (pj.first > lat) &&
                    lon < (pj.second - pi.second) * (lat - pi.first) / (pj.first - pi.first) + pi.second)
                    in = !in;
            }
            if (in) d = 0;
            else {
                d = 1e9;
                for (const auto& p : a.ring) d = std::min(d, haversineNm(lat, lon, p.first, p.second));
            }
        } else if (a.radiusNm > 0) {
            d = std::max(0.0, haversineNm(lat, lon, a.centreLat, a.centreLon) - a.radiusNm);
        } else {
            continue;
        }
        if (best < 0 || d < best) best = d;
    }
    return best;
}

// ---------------------------------------------------------------- merge

std::vector<Notam> merge(const std::vector<Snapshot>& snaps, double now, const MergeOptions& opt) {
    struct Holder { const Snapshot* s; const Notam* n; bool live; };
    std::map<std::string, std::vector<Holder>> byKey;
    auto liveOf = [&](const Snapshot& s) {
        const int i = (int)s.source;
        return s.ok && i >= 0 && i < kSources && now - s.asOfUtc <= opt.liveSec[i] && now - s.asOfUtc >= -300;
    };
    for (const Snapshot& s : snaps) {
        const bool live = liveOf(s);
        for (const Notam& n : s.items)
            if (!n.key.empty()) byKey[n.key].push_back({&s, &n, live});
    }
    static const Source kText[] = {Source::FaaApi, Source::TfrSite, Source::Fisb};
    static const Source kArea[] = {Source::TfrSite, Source::FaaApi, Source::Fisb};
    static const Source kTime[] = {Source::FaaApi, Source::TfrSite, Source::Fisb};
    // The first holder, in this order of sources, live ones before stale ones, for which `has` is true.
    auto pick = [](const std::vector<Holder>& hs, const Source* order, auto has) -> const Holder* {
        for (int pass = 0; pass < 2; pass++)
            for (int k = 0; k < kSources; k++)
                for (const Holder& h : hs)
                    if (h.live == (pass == 0) && h.s->source == order[k] && has(*h.n)) return &h;
        return nullptr;
    };

    std::vector<Notam> out;
    for (const auto& kv : byKey) {
        const std::vector<Holder>& hs = kv.second;
        double latest = 0;
        const Holder* newest = nullptr;
        bool anyLive = false;
        for (const Holder& h : hs) {
            const double seen = h.n->seenUtc > 0 ? h.n->seenUtc : h.s->asOfUtc;
            if (!newest || seen > latest) { latest = seen; newest = &h; }
            anyLive = anyLive || h.live;
        }
        if (newest->n->cancelled) continue;

        Notam m;
        m.key = kv.first;
        for (const Holder& h : hs) {
            m.sources |= (uint8_t)(1u << (int)h.s->source);
            if (m.number.empty()) m.number = h.n->number;
            if (m.kind.empty()) m.kind = h.n->kind;
            if (m.location.empty()) m.location = h.n->location;
            if (m.type.empty()) m.type = h.n->type;
        }
        // The kind and number as the most accurate source has them.
        if (const Holder* h = pick(hs, kText, [](const Notam& n) { return !n.kind.empty(); })) m.kind = h->n->kind;
        if (const Holder* h = pick(hs, kText, [](const Notam& n) { return !n.title.empty(); })) m.title = h->n->title;
        if (const Holder* h = pick(hs, kText, [](const Notam& n) { return !n.text.empty(); })) {
            m.text = h->n->text;
            m.textFrom = h->s->source;
            m.partStale = m.partStale || !h->live;
        }
        if (const Holder* h = pick(hs, kArea, [](const Notam& n) { return !n.areas.empty(); })) {
            m.areas = h->n->areas;
            m.areasFrom = h->s->source;
            m.partStale = m.partStale || !h->live;
        }
        if (const Holder* h = pick(hs, kTime, [](const Notam& n) { return n.startUtc > 0 || n.endUtc > 0 || n.permanent; })) {
            m.startUtc = h->n->startUtc;
            m.endUtc = h->n->endUtc;
            m.permanent = h->n->permanent;
            m.timesFrom = h->s->source;
            m.partStale = m.partStale || !h->live;
        }
        if (m.title.empty()) m.title = m.kind.empty() ? m.key : m.kind + " " + m.number;
        m.seenUtc = latest;
        m.ageSec = std::max(0.0, now - latest);
        m.stale = !anyLive;
        if (m.stale) m.partStale = false;   // all of it is stale, and says so once

        if (!m.permanent && m.endUtc > 0 && m.endUtc < now) continue;   // over

        // Gone: a live source that vouches for this kind (and this place) looked after every source that has it, and it was not there.
        bool gone = false;
        double lat = 0, lon = 0;
        const bool hasPos = positionOf(m, lat, lon);
        for (const Snapshot& s : snaps) {
            if (!s.complete || !liveOf(s) || s.asOfUtc <= latest) continue;
            if (!s.kinds.empty() && std::find(s.kinds.begin(), s.kinds.end(), m.kind) == s.kinds.end()) continue;
            if (s.hasRegion && (!hasPos || haversineNm(s.regionLat, s.regionLon, lat, lon) > s.regionNm)) continue;
            bool has = false;
            for (const Holder& h : hs) has = has || h.s == &s;
            if (!has) { gone = true; break; }
        }
        if (gone) continue;
        out.push_back(std::move(m));
    }
    std::sort(out.begin(), out.end(), [](const Notam& a, const Notam& b) {
        if (a.startUtc != b.startUtc) return a.startUtc < b.startUtc;
        return a.key < b.key;
    });
    return out;
}

// ---------------------------------------------------------------- tfr.faa.gov

bool parseTfrList(const std::string& body, double asOfUtc, Snapshot& out, std::string* err) {
    json::Value v;
    std::string e;
    if (!json::parse(body, v, e)) return fail(err, "the TFR list is not JSON");
    if (v.type != json::Value::Arr) return fail(err, "the TFR list is not a list");
    out = Snapshot();
    out.source = Source::TfrSite;
    out.ok = true;
    out.asOfUtc = asOfUtc;
    out.complete = true;       // tfr.faa.gov lists every TFR in the country
    out.kinds = {"TFR"};
    for (const json::Value& r : v.a) {
        if (r.type != json::Value::Obj) continue;
        const std::string num = normNumber(trim(r.str("notam_id")));
        if (num.empty()) continue;
        Notam n;
        n.key = "FDC " + num;
        n.number = trim(r.str("notam_id"));
        n.kind = "TFR";
        n.location = trim(r.str("facility"));
        n.type = trim(r.str("type"));
        const std::string desc = trim(r.str("description"));
        n.title = n.type.empty() ? desc : n.type + "  " + desc;
        n.seenUtc = asOfUtc;
        // The list's description is a summary, not the NOTAM: the text comes from the detail (or from FIS-B, until the detail is in).
        out.items.push_back(std::move(n));
    }
    return true;
}

std::string tfrListVersion(const std::string& body, const std::string& number) {
    json::Value v;
    std::string e;
    if (!json::parse(body, v, e) || v.type != json::Value::Arr) return {};
    const std::string want = normNumber(number);
    for (const json::Value& r : v.a)
        if (r.type == json::Value::Obj && normNumber(trim(r.str("notam_id"))) == want) return trim(r.str("mod_abs_time"));
    return {};
}

std::string tfrDetailUrl(const std::string& number) {
    // As the list writes it: "6/0100" is detail_6_0100.xml, not detail_6_100.
    std::string f = trim(number);
    if (normNumber(f).empty()) return {};
    std::replace(f.begin(), f.end(), '/', '_');
    return "https://tfr.faa.gov/download/detail_" + f + ".xml";
}

namespace {

std::shared_ptr<XmlNode> findDeep(const std::shared_ptr<XmlNode>& n, const std::string& name, int depth = 0) {
    if (!n || depth > 32) return nullptr;
    if (n->local() == name) return n;
    for (const auto& c : n->children)
        if (auto r = findDeep(c, name, depth + 1)) return r;
    return nullptr;
}

void findAllDeep(const std::shared_ptr<XmlNode>& n, const std::string& name, std::vector<std::shared_ptr<XmlNode>>& out, int depth = 0) {
    if (!n || depth > 32) return;
    if (n->local() == name) { out.push_back(n); return; }
    for (const auto& c : n->children) findAllDeep(c, name, out, depth + 1);
}

std::string textOf(const std::shared_ptr<XmlNode>& n, const std::string& child) {
    if (!n) return {};
    auto c = n->first(child);
    return c ? trim(c->text) : std::string();
}

// "33.19173051N", "083.46666667W"
bool coordOf(const std::string& s0, double& v) {
    const std::string s = trim(s0);
    if (s.size() < 2) return false;
    const char h = (char)toupper((unsigned char)s.back());
    char* end = nullptr;
    v = strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    if (h == 'S' || h == 'W') v = -v;
    else if (h != 'N' && h != 'E' && !isdigit((unsigned char)h)) return false;
    return std::isfinite(v);
}

// An aseTFRArea's limits: code ALT above sea level, HEI above ground, STD a flight level.
void limitsOf(const std::shared_ptr<XmlNode>& a, Area& out) {
    auto one = [&](const char* code, const char* val, const char* uom, int& ft, bool& agl) {
        const std::string v = textOf(a, val);
        if (v.empty()) return false;
        const std::string c = upper(textOf(a, code)), u = upper(textOf(a, uom));
        ft = atoi(v.c_str());
        if (u == "FL" || c == "STD") ft *= 100;
        else if (u == "M") ft = (int)lround(ft * 3.28084);
        agl = c == "HEI";
        return true;
    };
    const bool lo = one("codeDistVerLower", "valDistVerLower", "uomDistVerLower", out.floorFt, out.floorAgl);
    const bool hi = one("codeDistVerUpper", "valDistVerUpper", "uomDistVerUpper", out.ceilingFt, out.ceilingAgl);
    out.hasLimits = lo && hi;
}

}  // namespace

bool parseTfrDetail(const std::string& xml0, Notam& n, std::string* err) {
    std::string xml = xml0;
    if (xml.size() >= 3 && (uint8_t)xml[0] == 0xEF && (uint8_t)xml[1] == 0xBB && (uint8_t)xml[2] == 0xBF) xml.erase(0, 3);
    auto root = parseXml(xml);
    if (!root) return fail(err, "the TFR detail is not XML");
    auto no = findDeep(root, "Not");
    if (!no) return fail(err, "the TFR detail has no NOTAM in it");
    // The number from the year and the sequence: txtLocalName is often the number but sometimes a name ("261027 Flint & Howell, MI VIP").
    auto uid = no->first("NotUid");
    const std::string year = textOf(uid, "dateIndexYear"), seq = textOf(uid, "noSeqNo");
    std::string num = year.size() == 4 ? normNumber(year.substr(3) + "/" + seq) : std::string();
    if (num.empty()) num = normNumber(textOf(uid, "txtLocalName"));
    if (num.empty()) return fail(err, "the TFR detail has no NOTAM number");
    if (!n.key.empty() && n.key != "FDC " + num) return fail(err, "the TFR detail is for another NOTAM");
    n.key = "FDC " + num;
    if (n.number.empty()) n.number = num;
    n.kind = "TFR";
    if (n.location.empty()) n.location = textOf(no, "codeFacility");

    // Times: in the zone the detail names. One it does not name is left unknown rather than guessed.
    int off = 0;
    if (zoneOffset(textOf(no, "codeTimeZone"), off)) {
        const int64_t t = parseIsoUtc(textOf(no, "dateEffective"));
        if (t) n.startUtc = t - (int64_t)off * 3600;
    }
    int offE = off;
    const std::string ez = textOf(no, "codeExpirationTimeZone");
    if (ez.empty() ? zoneOffset(textOf(no, "codeTimeZone"), offE) : zoneOffset(ez, offE)) {
        const int64_t t = parseIsoUtc(textOf(no, "dateExpire"));
        if (t) n.endUtc = t - (int64_t)offE * 3600;
    }

    auto tfr = no->first("TfrNot");
    std::string text;
    std::vector<std::shared_ptr<XmlNode>> descr;
    findAllDeep(no, "txtDescrTraditional", descr);
    if (!descr.empty()) text = trim(descr[0]->text);
    if (text.empty()) {   // the nationwide notices carry only the USNS form
        descr.clear();
        findAllDeep(no, "txtDescrUSNS", descr);
        if (!descr.empty()) text = trim(descr[0]->text);
    }
    std::vector<std::shared_ptr<XmlNode>> purpose;
    findAllDeep(no, "txtDescrPurpose", purpose);
    if (!purpose.empty() && !trim(purpose[0]->text).empty()) {
        if (n.title.empty()) n.title = trim(purpose[0]->text);
    }
    if (!text.empty()) n.text = text;

    // The airspace: each aseTFRArea gives the limits, the abdMergedArea with its id the outline (already drawn as points, circles too).
    std::vector<std::shared_ptr<XmlNode>> ases, abds;
    findAllDeep(tfr ? tfr : no, "aseTFRArea", ases);
    findAllDeep(tfr ? tfr : no, "abdMergedArea", abds);
    std::map<std::string, std::shared_ptr<XmlNode>> aseById;
    for (const auto& a : ases) aseById[textOf(a->first("AseUid"), "codeId")] = a;
    std::vector<Area> areas;
    for (const auto& b : abds) {
        Area ar;
        for (const auto& v : b->all("Avx")) {
            double la, lo;
            if (coordOf(textOf(v, "geoLat"), la) && coordOf(textOf(v, "geoLong"), lo) && std::fabs(la) <= 90 && std::fabs(lo) <= 180)
                ar.ring.push_back({la, lo});
        }
        if (ar.ring.size() < 3) continue;
        auto au = b->first("AbdUid");
        const std::string id = au ? textOf(au->first("AseUid"), "codeId") : std::string();
        auto it = aseById.find(id);
        if (it != aseById.end()) limitsOf(it->second, ar);
        areas.push_back(std::move(ar));
    }
    if (!areas.empty()) n.areas = std::move(areas);
    return true;
}

// ---------------------------------------------------------------- the FAA NOTAM API

std::string faaApiUrl(double lat, double lon, double radiusNm, int page) {
    if (!std::isfinite(lat) || !std::isfinite(lon) || std::fabs(lat) > 90 || std::fabs(lon) > 180) return {};
    radiusNm = std::max(1.0, std::min(100.0, radiusNm));
    char b[256];
    snprintf(b, sizeof b,
             "https://external-api.faa.gov/notamapi/v1/notams?responseFormat=geoJson&locationLatitude=%.4f&locationLongitude=%.4f"
             "&locationRadius=%.0f&pageSize=1000&pageNum=%d",
             lat, lon, radiusNm, std::max(1, page));
    return b;
}

bool faaKeyValid(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    for (char c : s)
        if (c <= ' ' || c > '~' || c == '"' || c == '\'' || c == '\\') return false;
    return true;
}

namespace {

void geometryOf(const json::Value& g, double radiusNm, std::vector<Area>& out, int depth = 0) {
    if (g.type != json::Value::Obj || depth > 4) return;
    const std::string t = g.str("type");
    if (t == "GeometryCollection") {
        if (const json::Value* gs = g.get("geometries"))
            for (const json::Value& x : gs->a) geometryOf(x, radiusNm, out, depth + 1);
        return;
    }
    const json::Value* c = g.get("coordinates");
    if (!c || c->type != json::Value::Arr) return;
    auto ringOf = [&](const json::Value& r) {
        Area a;
        for (const json::Value& p : r.a)
            if (p.type == json::Value::Arr && p.a.size() >= 2 && p.a[0].type == json::Value::Num && p.a[1].type == json::Value::Num &&
                std::fabs(p.a[1].n) <= 90 && std::fabs(p.a[0].n) <= 180)
                a.ring.push_back({p.a[1].n, p.a[0].n});   // GeoJSON is lon, lat
        if (a.ring.size() >= 3) out.push_back(std::move(a));
    };
    if (t == "Polygon" && !c->a.empty()) {
        ringOf(c->a[0]);
    } else if (t == "MultiPolygon") {
        for (const json::Value& poly : c->a)
            if (poly.type == json::Value::Arr && !poly.a.empty()) ringOf(poly.a[0]);
    } else if (t == "Point" && radiusNm > 0 && c->a.size() >= 2 && c->a[0].type == json::Value::Num && c->a[1].type == json::Value::Num) {
        Area a;
        a.centreLat = c->a[1].n;
        a.centreLon = c->a[0].n;
        a.radiusNm = radiusNm;
        if (std::fabs(a.centreLat) <= 90 && std::fabs(a.centreLon) <= 180) out.push_back(a);
    }
}

}  // namespace

bool parseFaaApi(const std::string& body, double asOfUtc, std::vector<Notam>& out, int& totalPages, std::string* err) {
    json::Value v;
    std::string e;
    if (!json::parse(body, v, e)) return fail(err, "the NOTAM API answer is not JSON");
    const json::Value* items = v.get("items");
    if (!items || items->type != json::Value::Arr) return fail(err, "the NOTAM API answer has no items");
    totalPages = (int)v.num("totalPages", 1);
    for (const json::Value& it : items->a) {
        const json::Value* props = it.get("properties");
        const json::Value* core = props ? props->get("coreNOTAMData") : nullptr;
        const json::Value* no = core ? core->get("notam") : nullptr;
        if (!no) continue;
        const std::string num = normNumber(trim(no->str("number")));
        if (num.empty()) continue;
        const std::string cls = upper(trim(no->str("classification")));
        Notam n;
        n.number = trim(no->str("number"));
        n.location = trim(no->str("icaoLocation"));
        if (n.location.empty()) n.location = trim(no->str("location"));
        n.text = trim(no->str("text"));
        if (const json::Value* tr = core->get("notamTranslation"))
            for (const json::Value& t : tr->a)
                if (upper(t.str("type")) == "LOCAL_FORMAT") {
                    const std::string s = trim(t.str("simpleText").empty() ? t.str("formattedText") : t.str("simpleText"));
                    if (!s.empty()) n.text = s;
                }
        if (cls == "FDC") {
            n.key = "FDC " + num;
            n.kind = upper(n.text).find("TEMPORARY FLIGHT RESTRICTION") != std::string::npos ? "TFR" : "FDC";
        } else {
            std::string acct = upper(trim(no->str("accountId")));
            if (acct.empty()) acct = upper(trim(no->str("location")));
            if (acct.empty()) continue;
            n.key = "D " + acct + " " + num;
            n.kind = cls == "DOM" || cls.empty() ? "D" : cls;
        }
        n.cancelled = upper(trim(no->str("type"))) == "C";
        n.startUtc = parseIsoUtc(no->str("effectiveStart"));
        const std::string end = upper(trim(no->str("effectiveEnd")));
        if (end == "PERM") n.permanent = true;
        else n.endUtc = parseIsoUtc(end);
        n.title = n.kind + " " + n.number + (n.location.empty() ? "" : "  " + n.location);
        n.seenUtc = asOfUtc;
        const double radius = atof(no->str("radius").c_str());
        if (const json::Value* g = it.get("geometry")) geometryOf(*g, radius, n.areas);
        int lo = 0, hi = 0;
        bool loAgl = false, hiAgl = false;
        if (readLimit(no->str("lowerLimit"), lo, loAgl) && readLimit(no->str("upperLimit"), hi, hiAgl))
            for (Area& a : n.areas) { a.hasLimits = true; a.floorFt = lo; a.ceilingFt = hi; a.floorAgl = loAgl; a.ceilingAgl = hiAgl; }
        out.push_back(std::move(n));
    }
    return true;
}

// ---------------------------------------------------------------- FIS-B

Snapshot fromFisb(const std::vector<UatFisbProduct>& products, double signalToUtc, double asOfUtc) {
    Snapshot s;
    s.source = Source::Fisb;
    s.ok = true;
    s.asOfUtc = asOfUtc;
    s.complete = false;   // a ground station sends what is near it: what it has not sent says nothing
    for (const UatFisbProduct& p : products) {
        if (p.productId != 8 && p.productId != 16 && p.productId != 17) continue;
        Notam n;
        // The text names itself: "NOTAM-TFR 0/6733 ...", "NOTAM-D KFWA.07/077 240200Z !FWA 07/077 ...", "NOTAM-FDC 6/1234 ...".
        std::string first, second;
        {
            const size_t a = p.text.find(' ');
            first = p.text.substr(0, a);
            if (a != std::string::npos) {
                const size_t b = p.text.find(' ', a + 1);
                second = p.text.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            }
        }
        std::string kind = first.rfind("NOTAM-", 0) == 0 ? first.substr(6) : std::string();
        if (kind.empty()) kind = p.title.rfind("NOTAM-", 0) == 0 ? p.title.substr(6) : std::string();
        if (p.productId == 16) kind = "TRA";
        if (p.productId == 17) kind = "TMOA";
        if (kind == "TFR" || kind == "FDC") {
            std::string num = normNumber(second);
            // A TFR's graphic can come before its text: the report's own number and the last digit of its year name it.
            if (num.empty() && kind == "TFR" && p.reportNumber) num = std::to_string(p.reportYear % 10) + "/" + std::to_string(p.reportNumber);
            if (num.empty()) continue;
            n.key = "FDC " + num;
            n.number = num;
        } else if (kind == "D") {
            // By accountability ("!FWA 07/077"), which is what the API keys a NOTAM-D by too.
            const size_t bang = p.text.find('!');
            std::string acct, num;
            if (bang != std::string::npos) {
                const size_t sp = p.text.find(' ', bang);
                if (sp != std::string::npos) {
                    acct = upper(p.text.substr(bang + 1, sp - bang - 1));
                    const size_t e = p.text.find_first_of(" \n", sp + 1);
                    num = normNumber(p.text.substr(sp + 1, e == std::string::npos ? std::string::npos : e - sp - 1));
                    n.number = p.text.substr(sp + 1, e == std::string::npos ? std::string::npos : e - sp - 1);
                }
            }
            if (acct.empty() || num.empty()) continue;
            n.key = "D " + acct + " " + num;
            // "... 2007240200-2007240400" (or "-PERM") at its end: when it applies.
            const std::string t = trim(p.text);
            const size_t dash = t.rfind('-');
            if (dash != std::string::npos && dash >= 10) {
                n.startUtc = parseNotamTime(t.substr(dash - 10, 10));
                const std::string tail = upper(t.substr(dash + 1));
                if (tail.rfind("PERM", 0) == 0) n.permanent = true;
                else n.endUtc = parseNotamTime(tail);
                if (!n.startUtc) n.endUtc = 0, n.permanent = false;
            }
        } else if (!kind.empty() && p.reportNumber) {
            n.key = kind + " " + p.location + " " + std::to_string(p.reportNumber);
        } else {
            continue;
        }
        n.kind = kind;
        n.location = p.location;
        n.text = p.text;
        n.cancelled = p.cancelled;
        n.title = "NOTAM-" + kind + (n.number.empty() ? "" : " " + n.number) + (p.location.empty() ? "" : "  " + p.location);
        n.seenUtc = p.timeSec + signalToUtc;
        for (const UatFisbProduct::Area& a : p.areas) {
            Area b;
            b.ring = a.ring;
            b.centreLat = a.centreLat;
            b.centreLon = a.centreLon;
            b.radiusNm = a.radiusNm;
            b.hasLimits = true;
            b.floorFt = a.floorFt;
            b.ceilingFt = a.ceilingFt;
            b.floorAgl = b.ceilingAgl = a.agl;
            n.areas.push_back(std::move(b));
        }
        s.items.push_back(std::move(n));
    }
    return s;
}

}  // namespace dect2::notam
