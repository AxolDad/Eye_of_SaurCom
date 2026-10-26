// Asking an aggregator what is in the air: see adsb_api_app.h
//
// One thread does the asking, because the fetch helper blocks. It walks the enabled services, never faster than each one's own rate limit,
// writes the answer to a file in the cache folder (which is all the helper can do) and reads it back. What it finds goes to the receiver as a
// source of its own; the receiver shows it and nothing else does anything with it.
#include "adsb_api_app.h"
#include "../app.h"
#include "../state/receivers.h"
#include "dect2/adsb/adsb_api.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include "../i18n/i18n.h"
#include "../platform/platform.h"
#include "../ui/theme.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace adsbapi {
namespace {

struct Svc {
    std::string id;
    bool on = false;
    std::string key;
    double radiusNm = 60;
    double intervalSec = 10;
    // what the last attempt did, for the window
    double lastAt = 0;
    bool lastOk = false;
    size_t lastCount = 0, lastSkipped = 0;
    std::string lastError;
};

struct State {
    bool loaded = false;
    std::vector<Svc> svc;              // one per entry of dect2::apiServices(), in that order
    bool any = false;                  // at least one on: the thread runs
    double lat = 0, lon = 0;           // the receiver position the thread may use
    bool havePos = false;
    // the thread
    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::condition_variable cv;
    // what the thread produced, waiting to be handed to the receiver on the UI thread
    struct Batch {
        std::string id, name;
        std::vector<dect2::AdsbExtUpdate> aircraft;
        bool ok = false;
        std::string error;
    };
    std::vector<Batch> ready;
};
State S;

double nowSec() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string prefKey(const char* field, const std::string& id) { return std::string("adsbApi.") + id + "." + field; }

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    for (const dect2::ApiService& s : dect2::apiServices()) {
        Svc v;
        v.id = s.id;
        v.on = d.getB(prefKey("on", v.id).c_str(), false);
        v.key = d.getS(prefKey("key", v.id).c_str(), "");
        v.radiusNm = (double)d.getI(prefKey("radius", v.id).c_str(), 60);
        v.intervalSec = (double)d.getI(prefKey("interval", v.id).c_str(), (long)s.minIntervalSec * 2);
        if (v.radiusNm < 1) v.radiusNm = 1;
        if (v.radiusNm > s.maxRadiusNm) v.radiusNm = s.maxRadiusNm;
        if (v.intervalSec < s.minIntervalSec) v.intervalSec = s.minIntervalSec;
        // A key the user cannot have meant (or that an older version stored) never reaches an address.
        if (!dect2::apiKeyLooksSafe(v.key)) v.key.clear();
        S.svc.push_back(std::move(v));
    }
}

void save() {
    plat::Prefs& d = plat::prefs();
    for (const Svc& v : S.svc) {
        d.setB(prefKey("on", v.id).c_str(), v.on);
        d.setS(prefKey("key", v.id).c_str(), v.key);
        d.setI(prefKey("radius", v.id).c_str(), (long)v.radiusNm);
        d.setI(prefKey("interval", v.id).c_str(), (long)v.intervalSec);
    }
    d.flush();
}

// Reads a whole file the fetch helper just wrote. Bounded: a service that answers with something enormous is refused rather than parsed.
bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
    out.clear();
    FILE* f = dect2::u8fopen(path, "rb");
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

void threadBody() {
    std::vector<double> nextAt;
    while (!S.stop.load(std::memory_order_relaxed)) {
        std::vector<Svc> want;
        double lat = 0, lon = 0;
        bool havePos = false;
        {
            std::unique_lock<std::mutex> lk(S.mu);
            S.cv.wait_for(lk, std::chrono::milliseconds(400));
            if (S.stop.load(std::memory_order_relaxed)) return;
            want = S.svc;
            lat = S.lat;
            lon = S.lon;
            havePos = S.havePos;
        }
        nextAt.resize(want.size(), 0.0);
        for (size_t i = 0; i < want.size(); i++) {
            if (S.stop.load(std::memory_order_relaxed)) return;
            const Svc& v = want[i];
            if (!v.on) continue;
            const dect2::ApiService* s = dect2::apiServiceById(v.id);
            if (!s) continue;
            const double t = nowSec();
            if (t < nextAt[i]) continue;
            // Never faster than the service's own limit, whatever the setting says.
            nextAt[i] = t + std::max(v.intervalSec, s->minIntervalSec);

            State::Batch b;
            b.id = v.id;
            b.name = s->name;
            if (!havePos) {
                b.ok = false;
                b.error = "no receiver position: set it on the ADS-B screen";
            } else {
                std::string err;
                const std::string url = dect2::apiUrl(*s, lat, lon, v.radiusNm, v.key, &err);
                if (url.empty()) {
                    b.ok = false;
                    b.error = err.empty() ? "could not build the address" : err;
                } else {
                    const std::string file = plat::cacheDir() + "/adsbapi_" + v.id + ".json";
                    const std::string ua = std::string("ZorCom/") + ZORCOM_VERSION + " (ADS-B API)";
                    std::string body;
                    if (!plat::fetchUrl(url, file, ua)) {
                        b.ok = false;
                        b.error = "could not reach the service";
                    } else if (!readFile(file, body, 8u << 20)) {
                        b.ok = false;
                        b.error = "the answer was unreadable or far too large";
                    } else {
                        std::vector<dect2::ApiAircraft> v2;
                        size_t skipped = 0;
                        if (!dect2::parseApiResponse(s->shape, body, s->arrayKey, v2, &err, &skipped)) {
                            b.ok = false;
                            b.error = err;
                        } else {
                            b.ok = true;
                            b.aircraft.reserve(v2.size());
                            for (const dect2::ApiAircraft& a : v2) b.aircraft.push_back(dect2::apiToExtUpdate(a));
                        }
                    }
                }
            }
            std::lock_guard<std::mutex> lk(S.mu);
            if (S.ready.size() < 64) S.ready.push_back(std::move(b));
        }
    }
}

void startThread() {
    if (S.th.joinable()) return;
    S.stop.store(false);
    S.th = dect2::namedThread("zorcom-adsbapi", threadBody);
}

void stopThread() {
    if (!S.th.joinable()) return;
    S.stop.store(true);
    S.cv.notify_all();
    S.th.join();
}

// The services that are on, in the order the table lists them: the receiver gives each one a source slot in this order.
std::vector<std::string> enabledNames() {
    std::vector<std::string> v;
    for (const Svc& s : S.svc) {
        if (!s.on) continue;
        const dect2::ApiService* d = dect2::apiServiceById(s.id);
        v.push_back(d ? d->name : s.id);
    }
    return v;
}

}  // namespace

void tick(App& a) {
    if (a.rxId != 1) return;   // one set of services for the program, on RX 1, as the feeds are
    load();
    const bool any = !enabledNames().empty();
    if (any != S.any) {
        S.any = any;
        a.engine.adsb().setApiServices(enabledNames());
        if (any) startThread();
        else stopThread();
    }
    // Tell the thread where the receiver is; it may not touch the App.
    double lat = 0, lon = 0;
    receiverLocation(lat, lon);
    std::vector<State::Batch> ready;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        S.lat = lat;
        S.lon = lon;
        S.havePos = lat != 0 || lon != 0;
        ready.swap(S.ready);
    }
    for (State::Batch& b : ready) {
        a.engine.adsb().applyApiAircraft(b.name, b.aircraft, b.ok, b.error);
        for (Svc& s : S.svc) {
            if (s.id != b.id) continue;
            s.lastAt = nowSec();
            s.lastOk = b.ok;
            s.lastError = b.error;
            if (b.ok) s.lastCount = b.aircraft.size();
        }
    }
}

void shutdown() { stopThread(); }

void panel(App& a) {
    load();
    ImGui::PushID("adsbapi");
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Ask a network what is in the air, including where your antenna cannot reach. Every service is off until you "
                                 "turn it on. What arrives is shown on the map and in the table, marked with where it came from: it is never "
                                 "passed on to the feeds above, and never recorded in the flight history, which is a record of what this "
                                 "antenna heard."));
    ImGui::PopTextWrapPos();
    double lat = 0, lon = 0;
    receiverLocation(lat, lon);
    if (lat == 0 && lon == 0)
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Set the receiver position on the ADS-B screen first: these services are asked about a circle around you."));

    bool changed = false;
    const std::vector<dect2::ApiService>& all = dect2::apiServices();
    for (size_t i = 0; i < all.size() && i < S.svc.size(); i++) {
        const dect2::ApiService& d = all[i];
        Svc& v = S.svc[i];
        ImGui::PushID((int)i);
        const bool reachable = d.keyMode != dect2::ApiKeyMode::NeedsHeader;
        ImGui::BeginDisabled(!reachable);
        if (ImGui::Checkbox(d.name, &v.on)) changed = true;
        ImGui::EndDisabled();
        if (!d.openAccess) {
            ImGui::SameLine();
            ImGui::TextColored(pal::warnAmber(), "%s", tr("needs their permission"));
        }
        if (d.note && *d.note) {
            ImGui::Indent();
            ImGui::PushTextWrapPos(440 * gUi);
            ImGui::TextDisabled("%s", tr(d.note));
            ImGui::PopTextWrapPos();
            ImGui::Unindent();
        }
        if (v.on && reachable) {
            ImGui::Indent();
            int r = (int)v.radiusNm;
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::SliderInt(tr("Radius (nm)"), &r, 1, (int)d.maxRadiusNm)) { v.radiusNm = r; changed = true; }
            int iv = (int)v.intervalSec;
            ImGui::SetNextItemWidth(120 * gUi);
            if (ImGui::SliderInt(tr("Ask every (s)"), &iv, (int)d.minIntervalSec, 300)) { v.intervalSec = iv; changed = true; }
            if (d.keyMode == dect2::ApiKeyMode::Query || d.keyMode == dect2::ApiKeyMode::UserInfo) {
                char buf[208];
                snprintf(buf, sizeof buf, "%s", v.key.c_str());
                ImGui::SetNextItemWidth(220 * gUi);
                if (ImGui::InputTextWithHint(d.keyMode == dect2::ApiKeyMode::UserInfo ? tr("Account (user:password)") : tr("Key"),
                                             tr("optional"), buf, sizeof buf, ImGuiInputTextFlags_Password)) {
                    v.key = buf;
                    changed = true;
                }
                if (!v.key.empty() && !dect2::apiKeyLooksSafe(v.key))
                    ImGui::TextColored(pal::badRed(), "%s", tr("Only letters, digits and - _ . ~ : can be used here."));
            }
            // what the last attempt did
            if (v.lastAt <= 0) ImGui::TextDisabled("%s", tr("not asked yet"));
            else if (v.lastOk) ImGui::TextColored(pal::okGreen(), tr("%zu aircraft"), v.lastCount);
            else {
                ImGui::PushTextWrapPos(440 * gUi);
                ImGui::TextColored(pal::badRed(), "%s", v.lastError.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Unindent();
        }
        if (d.docsUrl && *d.docsUrl) {
            ImGui::Indent();
            if (ImGui::SmallButton(tr("Their documentation"))) dect2::openUrl(d.docsUrl);
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (changed) {
        save();
        const bool any = !enabledNames().empty();
        S.any = any;
        a.engine.adsb().setApiServices(enabledNames());
        if (any) startThread();
        else stopThread();
    }
    ImGui::PopID();
}

}  // namespace adsbapi
