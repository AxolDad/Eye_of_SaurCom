// The remote-control API inside the app (core/src/net/remote_api.cpp is the server): the host that carries the commands out on the main thread, the
// published telemetry, the preferences and the settings panel (in View > Data feeds). Off by default; RX 1 only.
#include "../app.h"
#include "../ui/typography.h"
#include "dect2/automation/engine.h"
#include "dect2/net/event_ring.h"
#include "dect2/net/remote_api.h"
#include "dect2/results/rx_json.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>

using namespace dect2;

namespace {

double steadyNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

const char* kindName(DeviceInfo::Kind k) {
    switch (k) {
    case DeviceInfo::Synthetic: return "synthetic";
    case DeviceInfo::File: return "file";
    case DeviceInfo::HackRF: return "hackrf";
    case DeviceInfo::Soapy: return "soapy";
    case DeviceInfo::Native: return "native";
    case DeviceInfo::Audio: return "audio";
    case DeviceInfo::Hamlib: return "hamlib";
    }
    return "unknown";
}

struct Host : RemoteApiHost {
    int evListener = 0;   // feeds dect2::globalEventRing() while the API is on (UI thread only)
    // the ring is made here, before this Host is complete: so it is destroyed after the State that holds the server, whose client threads read it
    Host() { (void)globalEventRing(); }
    ~Host() override { setExport(false); }
    // Events are serialised into the ring only while the API is on: an all-families listener makes every decoder build and write every event
    void setExport(bool on) {
        if (on && evListener <= 0) evListener = automation::global().addListener([](const automation::Event& e) { globalEventRing().push(e); }, (1u << automation::kFamilies) - 1);
        else if (!on && evListener > 0) { automation::global().removeListener(evListener); evListener = 0; }
    }
    size_t eventLines(uint64_t& cursor, std::vector<std::string>& lines, size_t max, uint64_t& missed) override { return globalEventRing().since(cursor, lines, max, missed); }
    uint64_t eventHead() override { return globalEventRing().head(); }
    // ---- main-thread hand-over: the server's threads queue a job, tick() (every frame) runs it
    struct Job {
        std::function<bool(App&, std::string&)> fn;
        std::mutex m;
        std::condition_variable cv;
        bool running = false, done = false, cancelled = false, ok = false;
        std::string err;
    };
    std::mutex qMu;
    std::deque<std::shared_ptr<Job>> q;
    std::atomic<double> lastTick{-1e9};
    std::atomic<bool> shutdown{false};
    // the published telemetry
    std::mutex snapMu;
    std::string telJson = "{\"running\":false,\"available\":false}";
    uint64_t telSeq = 0;

    bool run(const std::function<bool(App&, std::string&)>& fn, std::string& err) {
        if (shutdown) { err = "the application is closing"; return false; }
        if (steadyNow() - lastTick > 1.5) { err = "the application is not responding (is its window minimised or busy?)"; return false; }
        auto j = std::make_shared<Job>();
        j->fn = fn;
        { std::lock_guard<std::mutex> lk(qMu); if (q.size() > 64) { err = "too many requests waiting"; return false; } q.push_back(j); }
        std::unique_lock<std::mutex> lk(j->m);
        if (!j->cv.wait_for(lk, std::chrono::seconds(3), [&] { return j->done || shutdown.load(); })) {
            if (!j->running) { j->cancelled = true; err = "the application is busy, try again"; return false; }
            j->cv.wait(lk, [&] { return j->done; });   // it has started: wait for its result rather than report a failure for something that happens
        }
        if (!j->done) { err = "the application is closing"; return false; }
        err = j->err;
        return j->ok;
    }
    // for the *Json calls
    std::string query(const std::function<std::string(App&)>& f) {
        std::string out, err;
        const bool ok = run([&](App& a, std::string&) { out = f(a); return true; }, err);
        return ok ? out : "{\"error\":\"" + jsonEscape(err) + "\"}";
    }

    void tick(App& a, bool on) {
        lastTick = steadyNow();
        std::deque<std::shared_ptr<Job>> todo;
        { std::lock_guard<std::mutex> lk(qMu); todo.swap(q); }
        for (auto& j : todo) {
            std::unique_lock<std::mutex> lk(j->m);
            if (j->cancelled) continue;
            j->running = true;
            lk.unlock();
            std::string err;
            const bool ok = j->fn(a, err);
            lk.lock();
            j->ok = ok; j->err = err; j->done = true;
            j->cv.notify_all();
        }
        if (!on) return;
        const bool running = a.engine.running();
        const uint64_t pub = a.rx.seq * 2 + (running ? 1 : 0);
        std::lock_guard<std::mutex> lk(snapMu);
        if (pub == telSeq && telJson.size() > 40) return;
        telSeq = pub;
        std::string j = rxTelemetryJson(a.rx, &a.quality.report());
        telJson = std::string("{\"running\":") + (running ? "true" : "false") + ",\"available\":" + (a.rx.seq ? "true" : "false") + "," + j.substr(1);
    }
    void abort() {
        shutdown = true;
        std::lock_guard<std::mutex> lk(qMu);
        for (auto& j : q) { std::lock_guard<std::mutex> l2(j->m); j->cancelled = true; j->cv.notify_all(); }
        q.clear();
    }

    // ---- the API
    std::string statusJson() override {
        return query([](App& a) {
            const int di = std::max(0, std::min(a.devIdx, (int)a.devices.size() - 1));
            const DeviceInfo& d = a.devices[(size_t)di];
            const bool run = a.engine.running();
            std::string mode = "";
            for (int i = 0; i < kNumModes; i++) if (kModes[i].family == a.family && (!kModes[i].subId || subIndexOf(kModes[i].subId) == a.sub)) { mode = kModes[i].name; break; }
            const double gain = d.isGeneric() ? a.tune.gainDb : (double)(a.tune.lnaDb + a.tune.vgaDb + (a.tune.ampOn ? 14 : 0));
            char b[256];
            std::string o = std::string("{\"running\":") + (run ? "true" : "false") + ",\"receiver\":" + std::to_string(a.rxId) + ",\"deviceIndex\":" + std::to_string(a.devIdx) +
                            ",\"device\":\"" + jsonEscape(d.name) + "\",\"deviceDisplay\":\"" + jsonEscape(radioShownName(a, di)) + "\",\"deviceKind\":\"" + kindName(d.kind) + "\",\"family\":" + std::to_string(a.family) + ",\"mode\":\"" + jsonEscape(mode) + "\"";
            snprintf(b, sizeof b, ",\"frequencyHz\":%.0f,\"sampleRateHz\":%.0f,\"gainDb\":%s,\"agc\":%s,\"gainMinDb\":%s,\"gainMaxDb\":%s", a.freqMhz * 1e6,
                     run ? a.engine.radioRate() : a.tune.sampleRate, jsonNum(gain).c_str(), a.agcOn ? "true" : "false",
                     jsonNum(d.isGeneric() ? d.gainMinDb : 0).c_str(), jsonNum(d.isGeneric() ? d.gainMaxDb : (d.kind == DeviceInfo::HackRF ? 116 : 0)).c_str());
            o += b;
            // where this station is (My station), for the stations linked to it: only a real position, never the placeholder
            const dect2::geo::StationPos p = stationPos();
            if (p.valid()) {
                if (std::isfinite(p.altM)) snprintf(b, sizeof b, ",\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f", p.lat, p.lon, p.altM);
                else snprintf(b, sizeof b, ",\"lat\":%.6f,\"lon\":%.6f", p.lat, p.lon);
                o += b;
            }
            return o + "}";
        });
    }
    std::string devicesJson() override {
        return query([](App& a) {
            std::string o = "[";
            for (size_t i = 0; i < a.devices.size(); i++) {
                const DeviceInfo& d = a.devices[i];
                if (i) o += ',';
                o += "{\"index\":" + std::to_string(i) + ",\"name\":\"" + jsonEscape(d.name) + "\",\"kind\":\"" + kindName(d.kind) + "\",\"board\":\"" + jsonEscape(d.board) + "\"";
                if (d.kind != DeviceInfo::File) o += ",\"serial\":\"" + jsonEscape(d.serial) + "\"";   // a file's "serial" is its path: not given out
                if (const dect2::RadioRow* rr = d.isRadio() ? radioRow(a, (int)i) : nullptr) {   // the radio's identity: the user's name, USB port, and #n among look-alikes (0 = unique)
                    o += ",\"label\":\"" + jsonEscape(rr->label) + "\",\"usb\":\"" + jsonEscape(d.usbPath) + "\",\"dup\":" + std::to_string(rr->dupIndex) +
                         ",\"display\":\"" + jsonEscape(dect2::radioDisplayName(*rr)) + "\"";
                }
                o += ",\"minRateHz\":" + jsonNum(d.minRateHz) + ",\"maxRateHz\":" + jsonNum(d.maxRateHz) + ",\"gainMinDb\":" + jsonNum(d.gainMinDb) + ",\"gainMaxDb\":" + jsonNum(d.gainMaxDb) +
                     ",\"minFreqHz\":" + jsonNum(d.minFreqHz) + ",\"maxFreqHz\":" + jsonNum(d.maxFreqHz) + ",\"selected\":" + (a.devIdx == (int)i ? "true" : "false") + "}";
            }
            return o + "]";
        });
    }
    std::string modesJson() override {
        std::string o = "[";   // constant: no need to go through the main thread
        for (int i = 0; i < kNumModes; i++) {
            if (i) o += ',';
            o += "{\"family\":" + std::to_string(kModes[i].family) + ",\"name\":\"" + jsonEscape(kModes[i].name) + "\",\"sub\":" + (kModes[i].subId ? "\"" + jsonEscape(kModes[i].subId) + "\"" : "null") + "}";
        }
        return o + "]";
    }
    std::string telemetryJson(uint64_t& seq) override {
        std::lock_guard<std::mutex> lk(snapMu);
        seq = telSeq;
        return telJson;
    }
    std::string spectrumJson(int maxBins, uint64_t& seq) override {
        uint64_t sq = 0;
        std::string j = query([&](App& a) {
            if (!a.engine.running() || a.spec.dbfs.empty()) return std::string("{\"running\":false,\"dbfs\":[]}");
            sq = a.spec.seq;
            const std::vector<float>& s = a.spec.dbfs;
            const size_t n = s.size(), out = std::min<size_t>(n, (size_t)std::max(16, maxBins)), grp = (n + out - 1) / out;
            char b[200];
            snprintf(b, sizeof b, "{\"running\":true,\"seq\":%llu,\"centerHz\":%.0f,\"sampleRateHz\":%.0f,\"rmsDbfs\":%s,\"peak\":%s,\"clipFraction\":%s,\"bins\":%zu,\"dbfs\":[",
                     (unsigned long long)a.spec.seq, radioCenterMhz(a) * 1e6, a.engine.radioRate(), jsonNum(a.spec.stats.rmsDbfs).c_str(), jsonNum(a.spec.stats.peak).c_str(),
                     jsonNum(a.spec.stats.clipFraction).c_str(), (n + grp - 1) / grp);
            std::string o = b;
            for (size_t i = 0; i < n; i += grp) {   // the strongest bin of every group, so narrow carriers survive the decimation
                float m = s[i];
                for (size_t k = i + 1; k < std::min(n, i + grp); k++) m = std::max(m, s[k]);
                if (i) o += ',';
                snprintf(b, sizeof b, "%.1f", m);
                o += b;
            }
            return o + "]}";
        });
        seq = sq;
        return j;
    }
    bool start(int dev, std::string& err) override {
        return run([dev](App& a, std::string& e) {
            if (a.engine.running()) { e = "already running"; return false; }
            if (dev >= (int)a.devices.size()) { e = "no such device (see /api/v1/devices)"; return false; }
            if (dev >= 0) a.devIdx = dev;
            startReceiver(a);
            if (!a.engine.running()) { e = "could not start the receiver" + (a.startErr.empty() ? std::string(" (see the log)") : ": " + a.startErr); return false; }
            return true;
        }, err);
    }
    bool stop(std::string& err) override {
        return run([](App& a, std::string&) { if (a.engine.running()) a.engine.stop(); return true; }, err);
    }
    bool setMode(int family, const std::string& sub, std::string& err) override {
        return run([family, sub](App& a, std::string& e) {
            if (a.engine.running()) { e = "stop the receiver before switching mode"; return false; }
            bool known = false;
            for (int i = 0; i < kNumModes; i++) known = known || (kModes[i].family == family && (sub.empty() || (kModes[i].subId && sub == kModes[i].subId)));
            if (!known) { e = "no such mode (see /api/v1/modes)"; return false; }
            selectMode(a, family, sub.empty() ? nullptr : sub.c_str());
            return a.family == family;
        }, err);
    }
    bool setFrequencyHz(double hz, std::string& err) override {
        return run([hz](App& a, std::string& e) {
            if (!(hz >= 0)) { e = "the frequency must be zero or more"; return false; }   // (also NaN) the only refusal: no radio takes a negative frequency
            tuneFreq(a, hz / 1e6);
            return true;
        }, err);
    }
    bool setSampleRateHz(double hz, std::string& err) override {
        return run([hz](App& a, std::string& e) {
            const DeviceInfo& d = a.devices[(size_t)a.devIdx];
            if (d.kind == DeviceInfo::File) { e = "a recording plays at its own rate"; return false; }
            if (hz > 0 && d.isRadio() && ((d.maxRateHz > 0 && hz > d.maxRateHz * 1.0001) || (d.minRateHz > 0 && hz < d.minRateHz * 0.9999))) { e = "outside the sample rates of this radio"; return false; }
            saveSampleRate(a, hz);   // what the "Rate" control does
            const bool restart = a.engine.running() && d.isRadio();
            const double old = a.tune.sampleRate;
            a.tune.centerHz = a.freqMhz * 1e6;
            applyBandwidth(a);
            if (restart && a.tune.sampleRate != old) { startReceiver(a); a.mpd.reset(); a.quality.reset(); }
            return true;
        }, err);
    }
    bool setGainDb(double db, std::string& err) override {
        return run([db](App& a, std::string& e) {
            const DeviceInfo& d = a.devices[(size_t)a.devIdx];
            if (!d.isRadio()) { e = "this source has no gain control"; return false; }
            if (d.isGeneric()) {
                if (d.gainMaxDb > d.gainMinDb && (db < d.gainMinDb - 0.01 || db > d.gainMaxDb + 0.01)) { e = "outside the gain range of this radio"; return false; }
                a.tune.gainDb = std::round(db);
            } else {
                if (db < 0 || db > 116) { e = "the HackRF's total gain is 0 to 116 dB"; return false; }
                if (!a.tune.ampOn && db > 102) { e = "above 102 dB the amp is needed: switch it on yourself first (it can be damaged by strong signals)"; return false; }
                if (a.tune.ampOn && db < 14) { e = "the amp is on (+14 dB): the total is 14 dB or more"; return false; }
                const GainSetting g = agcModel(a).split((int)std::lround(db), a.tune.ampOn, 40);
                a.tune.lnaDb = g.lna; a.tune.vgaDb = g.vga; a.tune.ampOn = g.amp;
            }
            a.agcOn = false;   // a gain that was set by hand is not the AGC's to change
            if (a.engine.running()) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
            savePrefs(a);
            return true;
        }, err);
    }
};

struct State {
    Host host;
    RemoteApiServer srv{host};   // after the host: stopped first
    RemoteApiConfig cfg;
    ~State() { host.abort(); }   // connection threads waiting for the (gone) main thread are released before the server joins them
};
State& st() { static State s; return s; }

// the server and the event export follow the config together
void applyConfig(State& s) {
    s.srv.configure(s.cfg);
    s.host.setExport(s.cfg.on);
}

} // namespace

void remoteApiLoadPrefs(App& a) {
    if (a.rxId != 1) return;
    plat::Prefs& d = plat::prefs();
    RemoteApiConfig& c = st().cfg;
    c.on = d.getB("apiOn", false); c.port = (int)d.getI("apiPort", c.port); c.lan = d.getI("apiLan", 0) != 0; c.token = d.getS("apiToken", "");
    if (c.on) applyConfig(st());
}

void remoteApiShutdown() {
    State& s = st();
    s.host.abort();                        // client threads waiting for the main thread are released first
    s.srv.configure(RemoteApiConfig());    // off: the listener and every connection thread are stopped and joined
    s.host.setExport(false);
}

void remoteApiSavePrefs(const App& a) {
    if (a.rxId != 1) return;
    plat::Prefs& d = plat::prefs();
    const RemoteApiConfig& c = st().cfg;
    d.setB("apiOn", c.on); d.setI("apiPort", c.port); d.setI("apiLan", c.lan ? 1 : 0); d.setS("apiToken", c.token);
}

void remoteApiTick(App& a) {
    if (a.rxId != 1) return;
    State& s = st();
    s.host.tick(a, s.cfg.on);
}

void remoteApiSection(App& a) {
    State& s = st();
    RemoteApiConfig& c = s.cfg;
    typo::separatorText(tr("Remote control API (HTTP and WebSocket)"));
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Let scripts and other programs control this receiver"), &c.on);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    int port = c.port;
    ImGui::InputInt(tr("Port"), &port, 0, 0);
    c.port = std::max(1, std::min(65535, port));
    ch |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::Indent();
    if (ImGui::Checkbox(tr("Let other computers connect (LAN, needs the token)"), &c.lan)) { ch = true; if (c.lan && c.token.size() < 16) c.token = remoteApiNewToken(); }
    if (c.lan) {
        char b[96];
        snprintf(b, sizeof b, "%s", c.token.c_str());
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::InputText(tr("Token"), b, sizeof b)) c.token = b;
        ch |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("New token"))) { c.token = remoteApiNewToken(); ch = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy token"))) ImGui::SetClipboardText(c.token.c_str());
    }
    const RemoteApiStatus ss = s.srv.status();
    if (!c.on) ImGui::TextDisabled("%s", tr("off"));
    else if (!ss.listening) ImGui::TextColored(pal::badRed(), "%s", ss.lastError.empty() ? tr("starting") : ss.lastError.c_str());
    else {
        ImGui::TextColored(pal::okGreen(), tr("listening on %s port %d, %d HTTP and %d WebSocket client(s), %d requests, %d refused"), c.lan ? "all addresses" : "127.0.0.1", s.srv.config().port,
                           ss.httpClients, ss.wsClients, (int)ss.requests, (int)ss.rejected);
        if (!ss.lastError.empty()) ImGui::TextColored(pal::badRed(), "%s", ss.lastError.c_str());
    }
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Programs on this computer need no token; any other address must send it as 'Authorization: Bearer <token>'. The API lists the radios, starts and stops the receiver, "
                                 "sets mode, frequency, sample rate and gain, and gives the telemetry and the spectrum as JSON, also as a WebSocket stream. Endpoints: see README.md."));
    ImGui::PopTextWrapPos();
    ImGui::Unindent();
    if (ch) { applyConfig(s); remoteApiSavePrefs(a); savePrefs(a); }
}
