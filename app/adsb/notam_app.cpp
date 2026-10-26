// NOTAMs and TFRs: see notam_app.h.
//
// One thread does the fetching, because the fetch helper blocks: the TFR list every 15 minutes and each TFR's detail when the list says it
// changed; the FAA API every 15 minutes for the circle around the receiver. Everything else (FIS-B, the merge, the window) is on the UI thread.
#include "notam_app.h"
#include "adsb_map.h"
#include "../app.h"
#include "../state/receivers.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../ui/theme.h"
#include "../ui/typography.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>

namespace notamapp {
namespace nt = dect2::notam;
namespace {

double wallNow() { return (double)time(nullptr); }

struct SourceState {
    nt::Snapshot snap;
    std::string error;        // what the last attempt said, "" when it worked
    double lastTry = 0;
};

struct State {
    bool loaded = false;
    // settings
    bool tfrOn = false, apiOn = false, nmsOn = false, layer = true;
    std::string apiId, apiSecret;
    std::string nmsId, nmsSecret;
    int nmsEnv = 0;           // nt::NmsEnv: credentials are issued for one environment
    int apiRadius = 50;
    int nearNm = 100;         // the window lists what is within this of the receiver; 0 for everything
    char search[64] = "";
    // the thread
    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::condition_variable cv;
    SourceState tfr, api, nms;   // under mu
    // The settings as the thread sees them: copied in by tick() under mu, so the window can change its own freely.
    bool thTfrOn = false, thApiOn = false, thNmsOn = false;
    std::string thId, thSecret, thNmsId, thNmsSecret;
    int thNmsEnv = 0;
    double thRadius = 50;
    double lat = 0, lon = 0;  // the receiver, for the API's circle (under mu)
    bool havePos = false;
    bool wantNow = false;     // "fetch now" (under mu)
    // FIS-B, on the UI thread
    nt::Snapshot fisb;
    std::map<std::string, std::pair<uint32_t, double>> fisbSeen;   // product -> (times heard, wall time it was last heard)
    uint64_t lastFrames = 0;
    bool uatOn = false;
    // the merge
    std::vector<nt::Notam> merged;
    double mergedAt = 0;
    std::string sel;
};
State S;

std::string dir() { return plat::dataDir(); }
std::string path(const std::string& name) { return dir() + "/notam_" + name; }

bool readFile(const std::string& p, std::string& out, size_t maxBytes) {
    out.clear();
    FILE* f = dect2::u8fopen(p, "rb");
    if (!f) return false;
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (out.size() + n > maxBytes) { fclose(f); return false; }
        out.append(buf, n);
    }
    fclose(f);
    return true;
}

bool writeFile(const std::string& p, const std::string& data) {
    const std::string tmp = p + ".part";
    FILE* f = dect2::u8fopen(tmp, "wb");
    if (!f) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) { std::remove(tmp.c_str()); return false; }
    std::remove(p.c_str());
    return std::rename(tmp.c_str(), p.c_str()) == 0;
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.tfrOn = d.getB("notam.tfrSite", false);
    S.apiOn = d.getB("notam.faaApi", false);
    S.apiId = d.getS("notam.faaId", "");
    S.apiSecret = d.getS("notam.faaSecret", "");
    S.apiRadius = (int)std::max(5L, std::min(100L, d.getI("notam.faaRadius", 50)));
    S.nmsOn = d.getB("notam.nms", false);
    S.nmsId = d.getS("notam.nmsId", "");
    S.nmsSecret = d.getS("notam.nmsSecret", "");
    S.nmsEnv = (int)std::max(0L, std::min(2L, d.getI("notam.nmsEnv", 0)));
    if (!nt::faaKeyValid(S.nmsId)) S.nmsId.clear();
    if (!nt::faaKeyValid(S.nmsSecret)) S.nmsSecret.clear();
    S.nms.snap.source = nt::Source::Nms;
    S.layer = d.getB("notam.mapLayer", true);
    S.nearNm = (int)std::max(0L, std::min(1000L, d.getI("notam.nearNm", 100)));
    if (!nt::faaKeyValid(S.apiId)) S.apiId.clear();
    if (!nt::faaKeyValid(S.apiSecret)) S.apiSecret.clear();
    S.tfr.snap.source = nt::Source::TfrSite;
    S.api.snap.source = nt::Source::FaaApi;
    S.fisb.source = nt::Source::Fisb;
}

void save() {
    plat::Prefs& d = plat::prefs();
    d.setB("notam.tfrSite", S.tfrOn);
    d.setB("notam.faaApi", S.apiOn);
    d.setS("notam.faaId", S.apiId);
    d.setS("notam.faaSecret", S.apiSecret);
    d.setI("notam.faaRadius", S.apiRadius);
    d.setB("notam.nms", S.nmsOn);
    d.setS("notam.nmsId", S.nmsId);
    d.setS("notam.nmsSecret", S.nmsSecret);
    d.setI("notam.nmsEnv", S.nmsEnv);
    d.setB("notam.mapLayer", S.layer);
    d.setI("notam.nearNm", S.nearNm);
    d.flush();
}

// ---------------------------------------------------------------- tfr.faa.gov (on the thread)

// The details kept between fetches, by number, with the list version they were fetched at.
struct Detail { std::string version; nt::Notam n; };
std::map<std::string, Detail> gDetails;

nt::Snapshot tfrSnapshotFrom(const std::string& list, double asOf, std::string& err) {
    nt::Snapshot s;
    if (!nt::parseTfrList(list, asOf, s, &err)) return s;
    for (nt::Notam& n : s.items) {
        auto it = gDetails.find(n.key);
        if (it == gDetails.end()) continue;
        const std::string title = n.title;
        n = it->second.n;
        if (!title.empty()) n.title = title;
        n.seenUtc = asOf;
    }
    return s;
}

// From the files the last good fetch left: shown as stale until a fetch works.
void tfrLoadKept() {
    std::string list, asOfText;
    if (!readFile(path("tfr_list.json"), list, 4u << 20) || !readFile(path("tfr_asof.txt"), asOfText, 64)) return;
    nt::Snapshot probe;
    if (!nt::parseTfrList(list, 0, probe)) return;
    for (const nt::Notam& n : probe.items) {
        std::string xml;
        Detail d;
        d.n = n;
        std::string f = n.number;
        std::replace(f.begin(), f.end(), '/', '_');
        if (readFile(path("tfr_" + f + ".xml"), xml, 2u << 20) &&
            nt::parseTfrDetail(xml, d.n)) {
            d.version = nt::tfrListVersion(list, n.number);
            gDetails[n.key] = std::move(d);
        }
    }
    std::string err;
    nt::Snapshot s = tfrSnapshotFrom(list, atof(asOfText.c_str()), err);
    s.ok = false;   // kept, not fetched: stale until a fetch works
    std::lock_guard<std::mutex> lk(S.mu);
    if (S.tfr.snap.items.empty()) S.tfr.snap = std::move(s);
}

void tfrFetch(const std::string& ua) {
    const double t = wallNow();
    const std::string listFile = plat::cacheDir() + "/notam_tfr_list.json";
    std::string list, err;
    if (!plat::fetchUrl("https://tfr.faa.gov/tfrapi/getTfrList", listFile, ua) || !readFile(listFile, list, 4u << 20)) {
        err = "could not reach tfr.faa.gov";
    } else {
        nt::Snapshot probe;
        if (!nt::parseTfrList(list, t, probe, &err)) list.clear();
        // Each TFR's detail, when the list says it is new or changed. Politely: one at a time.
        for (const nt::Notam& n : probe.items) {
            if (S.stop.load(std::memory_order_relaxed)) return;
            const std::string ver = nt::tfrListVersion(list, n.number);
            auto it = gDetails.find(n.key);
            if (it != gDetails.end() && it->second.version == ver) continue;
            const std::string url = nt::tfrDetailUrl(n.number);
            if (url.empty()) continue;
            std::string f = n.number;
            std::replace(f.begin(), f.end(), '/', '_');
            const std::string tmp = plat::cacheDir() + "/notam_detail.xml";
            std::string xml;
            Detail d;
            d.n = n;
            if (plat::fetchUrl(url, tmp, ua) && readFile(tmp, xml, 2u << 20) && nt::parseTfrDetail(xml, d.n)) {
                d.version = ver;
                gDetails[n.key] = std::move(d);
                writeFile(path("tfr_" + f + ".xml"), xml);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        // Details of TFRs no longer listed are of no use.
        for (auto it = gDetails.begin(); it != gDetails.end();) {
            bool listed = false;
            for (const nt::Notam& n : probe.items) listed = listed || n.key == it->first;
            if (listed) ++it;
            else it = gDetails.erase(it);
        }
    }
    std::lock_guard<std::mutex> lk(S.mu);
    S.tfr.lastTry = t;
    if (!list.empty()) {
        std::string e2;
        nt::Snapshot s = tfrSnapshotFrom(list, t, e2);
        s.ok = true;
        S.tfr.snap = std::move(s);
        S.tfr.error.clear();
        writeFile(path("tfr_list.json"), list);
        writeFile(path("tfr_asof.txt"), std::to_string((long long)t));
    } else {
        S.tfr.snap.ok = false;   // what it had stays, as stale
        S.tfr.error = err.empty() ? "the TFR list could not be read" : err;
    }
}

// ---------------------------------------------------------------- the FAA API (on the thread)

void apiFetch(const std::string& ua, const std::string& id, const std::string& secret, double lat, double lon, double radius) {
    const double t = wallNow();
    std::vector<nt::Notam> items;
    std::string err;
    bool ok = true;
    int pages = 1;
    for (int page = 1; page <= pages && page <= 5 && ok; page++) {
        const std::string url = nt::faaApiUrl(lat, lon, radius, page);
        const std::string file = plat::cacheDir() + "/notam_api.json";
        std::string body;
        if (url.empty()) { ok = false; err = "no receiver position"; break; }
        if (!plat::fetchUrlHeaders(url, file, ua, {"client_id: " + id, "client_secret: " + secret})) {
            ok = false;
            err = "the FAA NOTAM API refused or could not be reached: check the client id and secret";
        } else if (!readFile(file, body, 16u << 20)) {
            ok = false;
            err = "the answer was unreadable or far too large";
        } else if (!nt::parseFaaApi(body, t, items, pages, &err)) {
            ok = false;
        }
        std::remove(file.c_str());   // it is the user's query: not kept in the cache
    }
    std::lock_guard<std::mutex> lk(S.mu);
    S.api.lastTry = t;
    if (ok) {
        nt::Snapshot s;
        s.source = nt::Source::FaaApi;
        s.ok = true;
        s.asOfUtc = t;
        // Complete for the circle asked about, as long as every page came.
        s.complete = pages <= 5;
        s.hasRegion = true;
        s.regionLat = lat;
        s.regionLon = lon;
        s.regionNm = radius;
        s.items = std::move(items);
        S.api.snap = std::move(s);
        S.api.error.clear();
    } else {
        S.api.snap.ok = false;
        S.api.error = err;
    }
}

// ---------------------------------------------------------------- NMS (on the thread)

std::string gNmsToken;
double gNmsTokenUntil = 0;
std::string gNmsTokenFor;   // the environment and id the token was issued for

void nmsFetch(const std::string& ua, int env, const std::string& id, const std::string& secret, double lat, double lon, double radius) {
    const double t = wallNow();
    const nt::NmsEnv e = (nt::NmsEnv)env;
    std::string err;
    bool ok = true;
    const std::string who = std::to_string(env) + "|" + id;
    if (gNmsTokenFor != who) { gNmsToken.clear(); gNmsTokenUntil = 0; }
    if (gNmsToken.empty() || t > gNmsTokenUntil - 60) {
        std::string reply;
        double exp = 0;
        // The secret goes in the Basic header, which goes to curl in a private file: never on a command line, never in the body.
        if (!plat::postRequestReply(nt::nmsTokenUrl(e), "grant_type=client_credentials", "application/x-www-form-urlencoded",
                                    {nt::nmsBasicAuthHeader(id, secret)}, ua, reply)) {
            ok = false;
            err = std::string("NMS refused the client id and secret, or could not be reached (") + nt::nmsEnvName(e) + ")";
        } else if (!nt::parseNmsToken(reply, gNmsToken, exp, &err)) {
            ok = false;
        } else {
            gNmsTokenUntil = t + exp;
            gNmsTokenFor = who;
        }
    }
    std::vector<nt::Notam> items;
    if (ok) {
        const std::string url = nt::nmsNotamsUrl(e, lat, lon, radius);
        const std::string file = plat::cacheDir() + "/notam_nms.json";
        std::string body;
        if (url.empty()) { ok = false; err = "no receiver position"; }
        else if (!plat::fetchUrlHeaders(url, file, ua, {"Authorization: Bearer " + gNmsToken, "nmsResponseFormat: GEOJSON", "Accept: application/json"})) {
            ok = false;
            err = "NMS did not answer the NOTAM query";
            gNmsToken.clear();   // perhaps the token: a new one next time
        } else if (!readFile(file, body, 32u << 20)) {
            ok = false;
            err = "the answer was unreadable or far too large";
        } else if (!nt::parseNms(body, t, items, &err)) {
            ok = false;
        }
        std::remove(file.c_str());
    }
    std::lock_guard<std::mutex> lk(S.mu);
    S.nms.lastTry = t;
    if (ok) {
        nt::Snapshot s;
        s.source = nt::Source::Nms;
        s.ok = true;
        s.asOfUtc = t;
        s.complete = true;   // everything within the circle asked about
        s.hasRegion = true;
        s.regionLat = lat;
        s.regionLon = lon;
        s.regionNm = radius;
        s.items = std::move(items);
        S.nms.snap = std::move(s);
        S.nms.error.clear();
    } else {
        S.nms.snap.ok = false;
        S.nms.error = err;
    }
}

void threadBody() {
    const std::string ua = std::string("OnAir/") + ONAIR_VERSION + " (NOTAMs)";
    tfrLoadKept();
    double nextTfr = 0, nextApi = 0, nextNms = 0;
    while (!S.stop.load(std::memory_order_relaxed)) {
        bool tfrOn, apiOn, nmsOn, havePos, now;
        std::string id, secret, nmsId, nmsSecret;
        int nmsEnv;
        double lat, lon, radius;
        {
            std::unique_lock<std::mutex> lk(S.mu);
            S.cv.wait_for(lk, std::chrono::seconds(1));
            if (S.stop.load(std::memory_order_relaxed)) return;
            tfrOn = S.thTfrOn;
            apiOn = S.thApiOn && !S.thId.empty() && !S.thSecret.empty();
            id = S.thId;
            secret = S.thSecret;
            nmsOn = S.thNmsOn;
            nmsId = S.thNmsId;
            nmsSecret = S.thNmsSecret;
            nmsEnv = S.thNmsEnv;
            lat = S.lat;
            lon = S.lon;
            havePos = S.havePos;
            radius = S.thRadius;
            now = S.wantNow;
            S.wantNow = false;
        }
        const double t = wallNow();
        if (now) nextTfr = nextApi = nextNms = 0;
        if (nmsOn && havePos && t >= nextNms) {
            nmsFetch(ua, nmsEnv, nmsId, nmsSecret, lat, lon, radius);
            std::lock_guard<std::mutex> lk(S.mu);
            nextNms = t + (S.nms.error.empty() ? 900 : 300);
        }
        if (tfrOn && t >= nextTfr) {
            tfrFetch(ua);
            std::lock_guard<std::mutex> lk(S.mu);
            nextTfr = t + (S.tfr.error.empty() ? 900 : 120);   // every 15 minutes; again in 2 when it failed
        }
        if (apiOn && havePos && t >= nextApi) {
            apiFetch(ua, id, secret, lat, lon, radius);
            std::lock_guard<std::mutex> lk(S.mu);
            nextApi = t + (S.api.error.empty() ? 900 : 300);
        }
    }
}

void startThread() {
    if (S.th.joinable()) return;
    S.stop.store(false);
    S.th = dect2::namedThread("onair-notam", threadBody);
}

void stopThread() {
    if (!S.th.joinable()) return;
    S.stop.store(true);
    S.cv.notify_all();
    S.th.join();
}

// ---------------------------------------------------------------- FIS-B (UI thread)

void fisbTick(double t) {
    // Any receiver on UAT 978 MHz.
    const dect2::UatTelemetry* u = nullptr;
    for (App* r : receiversAll())
        if (r && r->rx.standard == 84 && r->engine.running()) { u = &r->rx.uat; break; }
    S.uatOn = u != nullptr;
    if (!u) { S.fisb.ok = false; return; }
    if (u->frameCount != S.lastFrames) { S.lastFrames = u->frameCount; S.fisb.asOfUtc = t; }
    S.fisb.ok = S.fisb.asOfUtc > 0;
    // A product's wall time: when its "heard" count last went up. The tracker's own times are signal time.
    std::vector<UatFisbProduct> ps;
    for (const UatFisbProduct& p : u->fisb) {
        if (p.productId != 8 && p.productId != 16 && p.productId != 17) continue;
        const std::string k = std::to_string(p.productId) + "|" + std::to_string(p.reportNumber) + "|" + std::to_string(p.reportYear) + "|" + p.location;
        auto& seen = S.fisbSeen[k];
        if (seen.first != p.heard || seen.second <= 0) { seen.first = p.heard; seen.second = t; }
        UatFisbProduct q = p;
        q.timeSec = seen.second;
        ps.push_back(std::move(q));
    }
    if (S.fisbSeen.size() > 8192) S.fisbSeen.clear();
    const double asOf = S.fisb.asOfUtc;
    S.fisb = nt::fromFisb(ps, 0, asOf);
}

std::string whenText(const nt::Notam& n, double now) {
    auto fmt = [](int64_t t) {
        time_t tt = (time_t)t;
        struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        char b[32];
        strftime(b, sizeof b, "%d %b %H:%MZ", &g);
        return std::string(b);
    };
    if (n.permanent) return n.startUtc && n.startUtc > now ? "from " + fmt(n.startUtc) + ", permanent" : "permanent";
    if (n.startUtc && n.startUtc > now) return fmt(n.startUtc) + (n.endUtc ? " - " + fmt(n.endUtc) : "");
    if (n.endUtc) return "in force until " + fmt(n.endUtc);
    return n.startUtc ? "in force" : "times not known";
}

std::string ageText(double s) {
    char b[32];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else if (s < 172800) snprintf(b, sizeof b, "%.1f h", s / 3600);
    else snprintf(b, sizeof b, "%.0f days", s / 86400);
    return b;
}

std::string sourcesText(uint8_t bits) {
    std::string s;
    for (int i = 0; i < nt::kSources; i++)
        if (bits & (1u << i)) s += std::string(s.empty() ? "" : ", ") + nt::sourceName((nt::Source)i);
    return s;
}

bool inForce(const nt::Notam& n, double now) { return (!n.startUtc || n.startUtc <= now) && (n.permanent || !n.endUtc || n.endUtc > now); }

}  // namespace

// ---------------------------------------------------------------- public

void tick(App& a) {
    if (a.rxId != 1) return;
    load();
    const double t = wallNow();
    const bool wantThread = S.tfrOn || S.apiOn || S.nmsOn;
    if (wantThread) startThread();
    else stopThread();
    {
        double lat = 0, lon = 0;
        receiverLocation(lat, lon);
        std::lock_guard<std::mutex> lk(S.mu);
        S.lat = lat;
        S.lon = lon;
        S.havePos = receiverLocationSet() && (lat != 0 || lon != 0);
        S.thTfrOn = S.tfrOn;
        S.thApiOn = S.apiOn && nt::faaKeyValid(S.apiId) && nt::faaKeyValid(S.apiSecret);
        S.thId = S.apiId;
        S.thSecret = S.apiSecret;
        S.thRadius = S.apiRadius;
        S.thNmsOn = S.nmsOn && nt::faaKeyValid(S.nmsId) && nt::faaKeyValid(S.nmsSecret);
        S.thNmsId = S.nmsId;
        S.thNmsSecret = S.nmsSecret;
        S.thNmsEnv = S.nmsEnv;
    }
    if (t - S.mergedAt < 2) return;
    S.mergedAt = t;
    fisbTick(t);
    std::vector<nt::Snapshot> snaps;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.tfrOn) snaps.push_back(S.tfr.snap);   // a source the user turned off is not used, not even its kept answer
        if (S.apiOn) snaps.push_back(S.api.snap);
        if (S.nmsOn) snaps.push_back(S.nms.snap);
    }
    snaps.push_back(S.fisb);
    S.merged = nt::merge(snaps, t);
}

void shutdown() { stopThread(); }

bool mapLayerOn() { load(); return S.layer; }
void setMapLayer(bool on) { load(); S.layer = on; save(); }

std::vector<nt::Notam> forAirport(const std::string& ident, double lat, double lon, double nearNm) {
    std::vector<nt::Notam> v;
    std::string id = ident, alt;
    for (char& c : id) c = (char)toupper((unsigned char)c);
    if (id.size() == 4 && id[0] == 'K') alt = id.substr(1);   // KFWA is FWA in a NOTAM-D's accountability
    for (const nt::Notam& n : S.merged) {
        std::string loc = n.location;
        for (char& c : loc) c = (char)toupper((unsigned char)c);
        const bool named = !id.empty() && (loc == id || (!alt.empty() && (loc == alt || n.key.rfind("D " + alt + " ", 0) == 0)));
        const double d = nearNm > 0 ? nt::distanceNm(n, lat, lon) : -1;
        if (named || (d >= 0 && d <= nearNm)) v.push_back(n);
    }
    return v;
}

void drawMap(ImDrawList* dl) {
    if (!S.layer) return;
    const double now = wallNow();
    for (const nt::Notam& n : S.merged) {
        if (n.areas.empty()) continue;
        const bool force = inForce(n, now);
        const ImU32 col = n.stale ? IM_COL32(150, 150, 150, 200) : force ? IM_COL32(235, 70, 60, 230) : IM_COL32(235, 170, 50, 210);
        const ImU32 fill = n.stale ? IM_COL32(150, 150, 150, 28) : force ? IM_COL32(235, 70, 60, 34) : IM_COL32(235, 170, 50, 26);
        const bool sel = n.key == S.sel;
        const float w = (sel ? 3.f : 1.6f) * gUi;
        for (const nt::Area& ar : n.areas) {
            if (!ar.ring.empty()) {
                std::vector<ImVec2> pts;
                pts.reserve(ar.ring.size());
                for (const auto& p : ar.ring) pts.push_back(adsbmap::project(p.first, p.second));
                dl->AddPolyline(pts.data(), (int)pts.size(), col, ImDrawFlags_Closed, w);
            } else if (ar.radiusNm > 0) {
                const ImVec2 c = adsbmap::project(ar.centreLat, ar.centreLon);
                const ImVec2 e = adsbmap::project(ar.centreLat + ar.radiusNm / 60.0, ar.centreLon);
                const float r = std::fabs(e.y - c.y);
                if (r < 2 || r > 20000) continue;
                dl->AddCircleFilled(c, r, fill, 96);
                dl->AddCircle(c, r, col, 96, w);
            }
        }
    }
}

void window(App& a) {
    if (!a.notamOpen) return;
    load();
    ImGui::SetNextWindowSize(ImVec2(860 * gUi, 600 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(trw("NOTAMs and TFRs"), &a.notamOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    const double now = wallNow();
    bool changed = false;

    // The sources: what each said last, and how long ago.
    SourceState tfr, api, nms;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        tfr = S.tfr;
        api = S.api;
        nms = S.nms;
    }
    auto status = [&](const SourceState& s, bool on) {
        if (!on && s.snap.items.empty()) { ImGui::TextDisabled("%s", tr("off")); return; }
        if (s.snap.asOfUtc > 0) {
            const bool live = s.snap.ok && now - s.snap.asOfUtc <= 1800;
            ImGui::TextColored(live ? pal::okGreen() : pal::warnAmber(), tr("%zu, %s ago%s"), s.snap.items.size(), ageText(now - s.snap.asOfUtc).c_str(),
                               live ? "" : tr(" (stale: shown only where nothing newer has it)"));
        } else if (on) {
            ImGui::TextDisabled("%s", tr("fetching ..."));
        }
        if (!s.error.empty()) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "%s", s.error.c_str()); }
    };
    typo::separatorText(tr("Sources, most accurate first"));
    ImGui::PushTextWrapPos(820 * gUi);
    ImGui::TextDisabled("%s", tr("Each NOTAM is shown from the most accurate source that has it now: the FAA's new NOTAM Management Service, "
                                 "then the older FAA NOTAM API, then tfr.faa.gov (which draws a TFR's airspace exactly), then FIS-B off the air. "
                                 "When a source cannot be reached its last answer is still used, marked as stale with its age, and never passed "
                                 "off as current."));
    ImGui::PopTextWrapPos();
    ImGui::PushID("nms");
    if (ImGui::Checkbox(tr("FAA NOTAM Management Service (NMS)"), &S.nmsOn)) changed = true;
    ImGui::SameLine();
    status(nms, S.nmsOn);
    if (S.nmsOn) {
        ImGui::Indent();
        static const char* kEnv[] = {"production", "staging", "fit"};
        ImGui::SetNextItemWidth(140 * gUi);
        if (ImGui::Combo(tr("Environment"), &S.nmsEnv, kEnv, 3)) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The FAA issues credentials for one environment; they do not work in another."));
        char b[136];
        snprintf(b, sizeof b, "%s", S.nmsId.c_str());
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::InputTextWithHint(tr("client id##nms"), tr("from the FAA"), b, sizeof b, ImGuiInputTextFlags_Password)) { S.nmsId = b; changed = true; }
        snprintf(b, sizeof b, "%s", S.nmsSecret.c_str());
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::InputTextWithHint(tr("client secret##nms"), tr("from the FAA"), b, sizeof b, ImGuiInputTextFlags_Password)) { S.nmsSecret = b; changed = true; }
        if ((!S.nmsId.empty() && !nt::faaKeyValid(S.nmsId)) || (!S.nmsSecret.empty() && !nt::faaKeyValid(S.nmsSecret)))
            ImGui::TextColored(pal::badRed(), "%s", tr("A key has spaces, quotes or other characters a key does not have."));
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderInt(tr("Radius (nm)##nms"), &S.apiRadius, 5, 100)) changed = true;
        if (!receiverLocationSet()) ImGui::TextColored(pal::warnAmber(), "%s", tr("Set the receiver position on the ADS-B screen: NMS is asked about a circle around it."));
        ImGui::TextDisabled("%s", tr("Access is by request to the FAA (notams@faa.gov). The same radius is used for the older API."));
        ImGui::Unindent();
    }
    ImGui::PopID();
    ImGui::PushID("api");
    if (ImGui::Checkbox(tr("FAA NOTAM API (older)"), &S.apiOn)) changed = true;
    ImGui::SameLine();
    status(api, S.apiOn);
    if (S.apiOn) {
        ImGui::Indent();
        char b[136];
        snprintf(b, sizeof b, "%s", S.apiId.c_str());
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::InputTextWithHint(tr("client_id"), tr("from api.faa.gov"), b, sizeof b, ImGuiInputTextFlags_Password)) { S.apiId = b; changed = true; }
        snprintf(b, sizeof b, "%s", S.apiSecret.c_str());
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::InputTextWithHint(tr("client_secret"), tr("from api.faa.gov"), b, sizeof b, ImGuiInputTextFlags_Password)) { S.apiSecret = b; changed = true; }
        if ((!S.apiId.empty() && !nt::faaKeyValid(S.apiId)) || (!S.apiSecret.empty() && !nt::faaKeyValid(S.apiSecret)))
            ImGui::TextColored(pal::badRed(), "%s", tr("A key has spaces, quotes or other characters a key does not have."));
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderInt(tr("Radius (nm)"), &S.apiRadius, 5, 100)) changed = true;
        if (!receiverLocationSet()) ImGui::TextColored(pal::warnAmber(), "%s", tr("Set the receiver position on the ADS-B screen: the API is asked about a circle around it."));
        if (ImGui::SmallButton(tr("Get a free key"))) dect2::openUrl("https://api.faa.gov/s/");
        ImGui::Unindent();
    }
    ImGui::PopID();
    ImGui::PushID("tfr");
    if (ImGui::Checkbox("tfr.faa.gov", &S.tfrOn)) changed = true;
    ImGui::SameLine();
    status(tfr, S.tfrOn);
    ImGui::PopID();
    ImGui::TextUnformatted(tr("FIS-B (UAT 978 MHz)"));
    ImGui::SameLine();
    if (!S.uatOn) ImGui::TextDisabled("%s", tr("no UAT receiver running: tune one to 978 MHz to hear the NOTAMs nearby ground stations send"));
    else ImGui::TextColored(pal::okGreen(), tr("%zu heard, last frame %s ago"), S.fisb.items.size(), ageText(now - S.fisb.asOfUtc).c_str());
    if (ImGui::Button(tr("Fetch now"))) { std::lock_guard<std::mutex> lk(S.mu); S.wantNow = true; S.cv.notify_all(); }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Show TFRs on the ADS-B map"), &S.layer)) changed = true;

    // The list.
    typo::separatorText(tr("NOTAMs"));
    double rlat = 0, rlon = 0;
    receiverLocation(rlat, rlon);
    const bool pos = receiverLocationSet();
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderInt(tr("Within (nm, 0 = all)"), &S.nearNm, 0, 500)) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint("##search", tr("search text, number, place"), S.search, sizeof S.search);
    std::string q = S.search;
    for (char& c : q) c = (char)toupper((unsigned char)c);

    if (ImGui::BeginTable("##notams", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                          ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("NOTAM"), ImGuiTableColumnFlags_WidthFixed, 130 * gUi);
        ImGui::TableSetupColumn(tr("What"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("When"), ImGuiTableColumnFlags_WidthFixed, 190 * gUi);
        ImGui::TableSetupColumn(tr("Distance"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
        ImGui::TableSetupColumn(tr("From"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
        ImGui::TableSetupColumn(tr("Age"), ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
        ImGui::TableHeadersRow();
        size_t shown = 0, hidden = 0;
        for (const nt::Notam& n : S.merged) {
            const double d = pos ? nt::distanceNm(n, rlat, rlon) : -1;
            if (S.nearNm > 0 && pos && d > S.nearNm) { hidden++; continue; }
            if (!q.empty()) {
                std::string hay = n.key + " " + n.title + " " + n.location + " " + n.text;
                for (char& c : hay) c = (char)toupper((unsigned char)c);
                if (hay.find(q) == std::string::npos) { hidden++; continue; }
            }
            shown++;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const std::string label = (n.kind.empty() ? std::string() : n.kind + " ") + (n.number.empty() ? n.key : n.number);
            if (ImGui::Selectable((label + "##" + n.key).c_str(), S.sel == n.key, ImGuiSelectableFlags_SpanAllColumns)) S.sel = S.sel == n.key ? "" : n.key;
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(n.title.c_str());
            ImGui::TableNextColumn();
            const bool force = inForce(n, now);
            if (force) ImGui::TextColored(pal::badRed(), "%s", whenText(n, now).c_str());
            else ImGui::TextUnformatted(whenText(n, now).c_str());
            ImGui::TableNextColumn();
            if (d < 0) ImGui::TextDisabled("-");
            else if (d == 0) ImGui::TextColored(pal::badRed(), "%s", tr("inside"));
            else ImGui::Text("%.0f", d);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(nt::sourceName(n.textFrom));
            ImGui::TableNextColumn();
            if (n.stale) ImGui::TextColored(pal::warnAmber(), tr("stale %s"), ageText(n.ageSec).c_str());
            else ImGui::TextDisabled("%s", ageText(n.ageSec).c_str());
        }
        ImGui::EndTable();
        ImGui::TextDisabled(tr("%zu shown, %zu filtered out"), shown, hidden);
        if (!pos && S.nearNm > 0) {
            ImGui::SameLine();
            ImGui::TextColored(pal::warnAmber(), "%s", tr("(no receiver position: the distance filter is off)"));
        }
    }

    // The one selected, whole.
    for (const nt::Notam& n : S.merged) {
        if (n.key != S.sel) continue;
        ImGui::BeginChild("##detail", ImVec2(0, 0), true);
        ImGui::TextUnformatted(n.title.c_str());
        ImGui::TextDisabled(tr("%s · held by %s · text from %s"), whenText(n, now).c_str(), sourcesText(n.sources).c_str(), nt::sourceName(n.textFrom));
        if (n.stale)
            ImGui::TextColored(pal::warnAmber(), tr("Stale: no source reachable now has this. Last had %s ago."), ageText(n.ageSec).c_str());
        else if (n.partStale)
            ImGui::TextColored(pal::warnAmber(), "%s", tr("Part of this (the airspace or the times) is from a source that is not up to date."));
        for (const nt::Area& ar : n.areas) {
            std::string shape = ar.ring.empty() ? tr("circle") : tr("outline");
            char b[160];
            if (ar.ring.empty()) snprintf(b, sizeof b, "%s %.1f NM", shape.c_str(), ar.radiusNm);
            else snprintf(b, sizeof b, "%s, %zu points", shape.c_str(), ar.ring.size());
            std::string lim;
            if (ar.hasLimits) {
                char l[96];
                snprintf(l, sizeof l, "  %s - %d ft %s", ar.floorFt == 0 && ar.floorAgl ? "SFC" : (std::to_string(ar.floorFt) + " ft" + (ar.floorAgl ? " AGL" : " MSL")).c_str(),
                         ar.ceilingFt, ar.ceilingAgl ? "AGL" : "MSL");
                lim = l;
            }
            ImGui::TextDisabled(tr("airspace (%s): %s%s"), nt::sourceName(n.areasFrom), b, lim.c_str());
        }
        ImGui::Separator();
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(n.text.empty() ? tr("(no text yet: only the list entry or the airspace has come)") : n.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        break;
    }
    if (changed) {
        save();
        std::lock_guard<std::mutex> lk(S.mu);
        S.wantNow = true;
        S.cv.notify_all();
    }
    ImGui::End();
}

}  // namespace notamapp
