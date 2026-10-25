// Live transcripts in the app. See transcribe_app.h and docs/TRANSCRIBE.md.
#include "dect2/common/thread_name.h"
#include "transcribe_app.h"
#include "dect2/airports/airports.h"
#include "dect2/transcribe/context.h"
#include "app.h"
#include "adsb/adsb_acars.h"
#include "adsb/adsb_internal.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include "dect2/transcribe/audio_tap.h"
#include "dect2/transcribe/captions.h"
#include "dect2/transcribe/models.h"
#include "dect2/transcribe/transcriber.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "state/receivers.h"
#include "ui/theme.h"
#include "ui/ui_common.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <map>
#include <mutex>
#include <thread>

namespace tx {

namespace {

struct Rx1 {   // the settings are the program's, not one receiver's
    int old;
    Rx1() : old(plat::gPrefRx) { plat::gPrefRx = 1; }
    ~Rx1() { plat::gPrefRx = old; }
};

struct Cfg {
    bool enabled = false;       // the master switch: off until the user turns it on
    bool airband = true;        // with the master switch on: the airband channels
    bool marine = false;        // marine VHF voice (off: it can be private)
    bool other = false;         // the sound of any other mode (amateur, CB, FM ...): off
    std::string model = "tiny.en";   // a catalogue id, or "file:<name>" for a file the user put in the models folder
    std::string lang = "en";    // "auto" needs a multilingual model
    bool translate = false;     // also keep an English translation of foreign speech (multilingual model)
    int threads = 2;            // CPU budget: threads of the speech engine
    int days = 7;               // retention
    // what the speech engine is given, which is not what you hear (dect2/transcribe/asr_front.h, docs/TRANSCRIBE.md)
    bool front = true;          // condition the audio for the engine
    bool frontBand = true;      // 300 - 3400 Hz: CTCSS, DCS and hiss out
    bool frontBurst = true;     // mute MDC-1200 and single-tone key-ups before the speech detector sees them
    bool bias = true;           // tell the engine the names that are on the air now (dect2/transcribe/context.h)
    // auto subtitles
    bool cap = false;           // master switch for captions (separate from the transcripts: nothing is kept in a file)
    std::set<int> capModes;     // the standards it runs for (empty: none)
    int capSource = 3;          // dect2::CaptionSource: TV subtitles from the station / generated / both / generated only where the station sends none
    int capSize = 100, capBg = 75, capLines = 2;
    bool capTop = false;
    int capShow = 0;            // 0 as spoken, 1 English translation when there is one, 2 both
    bool capSave = true;        // save .srt and .vtt next to an audio recording
    bool loaded = false;
};
Cfg C;

dect2::TranscriptStore gStore;
dect2::Transcriber gTr(gStore);
dect2::TranscriptStore gCapStore;             // captions: in memory only, never written to a file, never shared
dect2::Transcriber gCap(gCapStore);
std::map<int, std::unique_ptr<dect2::CaptionTrack>> gTracks;   // per receiver (guarded by gMu)
struct CapCtx { bool valid = false; int rx = 0, std = -1; double freqHz = 0; } gCapCtx;
dect2::ModelDownloader gDl;
bool gWindowOpen = false;
bool gStarted = false;

std::mutex gMu;                               // the maps below, read by the tap threads
std::map<int, std::vector<dect2::SourceInfo>> gChanInfo;   // per receiver: the airband channels by index
std::map<std::string, std::string> gAircraft;              // normalised callsign or registration > ICAO address (hex), for the resolver
struct Ctx { bool valid = false; std::string mode, domain, channel; double freqHz = 0; } gCtx;   // what the sound of the other modes is
std::map<int, bool> gTapOn;                   // airband tap installed, per receiver
bool gSinkOn = false;

// speech engine loading, in the background
std::thread gLoadTh;
std::atomic<int> gLoadState{0};               // 0 none, 1 loading, 2 ready, 3 failed
std::string gLoadErr, gLoadedFile, gLoadingFile;
std::shared_ptr<dect2::SpeechBackend> gPending;
std::shared_ptr<dect2::SpeechBackend> gBackend;
bool gBackendEnglishOnly = true;

std::string gModelsDirCached;
std::vector<std::string> gUserFiles;
double gUserFilesAt = -100;

void save();
void load() {
    if (C.loaded) return;
    C.loaded = true;
    Rx1 r;
    plat::Prefs& d = plat::prefs();
    C.enabled = d.getB("txEnabled", false);
    C.airband = d.getB("txAirband", true);
    C.marine = d.getB("txMarine", false);
    C.other = d.getB("txOther", false);
    C.model = d.getS("txModel", "tiny.en");
    C.lang = d.getS("txLang", "en");
    C.translate = d.getB("txTranslate", false);
    C.threads = (int)std::max(1L, std::min(8L, d.getI("txThreads", 2)));
    C.days = (int)std::max(1L, std::min(365L, d.getI("txDays", 7)));
    C.front = d.getB("txFront", true);
    C.frontBand = d.getB("txFrontBand", true);
    C.frontBurst = d.getB("txFrontBurst", true);
    C.bias = d.getB("txBias", true);
    C.cap = d.getB("capOn", false);
    {
        std::stringstream ss(d.getS("capModes", ""));
        std::string t;
        while (std::getline(ss, t, ',')) if (!t.empty() && dect2::captionStandardEligible(atoi(t.c_str()))) C.capModes.insert(atoi(t.c_str()));
    }
    C.capSource = (int)std::max(0L, std::min(3L, d.getI("capSource", 3)));
    C.capSize = (int)d.getI("capSize", 100); C.capBg = (int)d.getI("capBg", 75); C.capLines = (int)d.getI("capLines", 2);
    C.capTop = d.getB("capTop", false);
    C.capShow = (int)std::max(0L, std::min(2L, d.getI("capShow", 0)));
    C.capSave = d.getB("capSave", true);
}
void save() {
    Rx1 r;
    plat::Prefs& d = plat::prefs();
    d.setB("txEnabled", C.enabled); d.setB("txAirband", C.airband); d.setB("txMarine", C.marine); d.setB("txOther", C.other);
    d.setS("txModel", C.model); d.setS("txLang", C.lang); d.setB("txTranslate", C.translate); d.setI("txThreads", C.threads); d.setI("txDays", C.days);
    d.setB("txFront", C.front); d.setB("txFrontBand", C.frontBand); d.setB("txFrontBurst", C.frontBurst); d.setB("txBias", C.bias);
    d.setB("capOn", C.cap); d.setI("capSource", C.capSource); d.setI("capSize", C.capSize); d.setI("capBg", C.capBg); d.setI("capLines", C.capLines);
    d.setB("capTop", C.capTop); d.setI("capShow", C.capShow); d.setB("capSave", C.capSave);
    { std::string m; for (int k : C.capModes) m += (m.empty() ? "" : ",") + std::to_string(k); d.setS("capModes", m); }
    d.flush();
}

std::string modelsDir() {
    if (gModelsDirCached.empty()) gModelsDirCached = plat::dataDir() + "/models";
    return gModelsDirCached;
}

int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// the model file name for the setting, "" when there is none
std::string modelFile(const std::string& setting) {
    if (setting.compare(0, 5, "file:") == 0) return setting.substr(5);
    if (const dect2::SpeechModel* m = dect2::findSpeechModel(setting)) return m->file;
    return "";
}
bool modelPresent(const std::string& setting) {
    if (setting.compare(0, 5, "file:") == 0) {
        std::error_code ec;
        return std::filesystem::is_regular_file(dect2::u8file(modelsDir() + "/" + setting.substr(5)), ec);
    }
    const dect2::SpeechModel* m = dect2::findSpeechModel(setting);
    return m && dect2::speechModelInstalled(modelsDir(), *m);
}

void applyConfig() {
    dect2::TranscribeConfig c = gTr.config();
    c.enabled = C.enabled;
    c.workers = 1;
    c.threads = C.threads;
    c.language = C.lang;
    c.translate = C.translate;
    c.queueMax = 6;
    c.front.enabled = C.front;
    c.front.bandpass = C.frontBand;
    c.front.blankBursts = C.frontBurst;
    c.biasFromContext = C.bias;
    gTr.configure(c);
    gStore.setLimits(20000, C.days);
    dect2::TranscribeConfig k = gCap.config();   // captions: the same language and CPU budget, one worker, no music, a short queue (late captions are useless)
    k.enabled = C.cap;
    k.workers = 1;
    k.threads = C.threads;
    k.language = C.lang;
    k.translate = C.translate;
    k.queueMax = 3;
    k.segmenter.rejectMusic = true;
    k.segmenter.maxSec = 12;     // shorter pieces: the first words show up sooner
    k.segmenter.hangSec = 0.5;
    // Captions come from broadcast sound, not from a radio channel: there is no CTCSS or key-up signalling in it, and the live names of
    // aircraft and talkgroups have nothing to do with what is on television.
    k.front.enabled = false;
    k.biasFromContext = false;
    gCap.configure(k);
    gCapStore.setLimits(2000, 1);
}

// ---------------------------------------------------------------- the taps

void onAirband(int rxId, int chan, const float* x, size_t n) {
    dect2::SourceInfo si;
    {
        std::lock_guard<std::mutex> lk(gMu);
        const auto it = gChanInfo.find(rxId);
        if (it == gChanInfo.end() || chan < 0 || (size_t)chan >= it->second.size()) return;
        si = it->second[(size_t)chan];
    }
    gTr.feed(si, x, n, 8000, wallMs());
}

void onCaptionSound(const float* mono, size_t n, int rate);

void onSound(const float* mono, size_t n, int rate) {
    onCaptionSound(mono, n, rate);
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gCtx.valid) return;
    }
    dect2::SourceInfo si;
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gCtx.valid) return;
        si.key = 0x7000000000000LL; si.freqHz = gCtx.freqHz; si.mode = gCtx.mode; si.channel = gCtx.channel; si.domain = gCtx.domain;
    }
    gTr.feed(si, mono, n, rate, wallMs());
}

// the sound of the receiver being heard, for the captions: the audio clock first, then the speech
void onCaptionSound(const float* mono, size_t n, int rate) {
    dect2::SourceInfo si;
    dect2::CaptionTrack* tk = nullptr;
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gCapCtx.valid) return;
        auto& t = gTracks[gCapCtx.rx];
        if (!t) t = std::make_unique<dect2::CaptionTrack>();
        tk = t.get();
        si.key = 0x7100000000000LL + gCapCtx.rx; si.freqHz = gCapCtx.freqHz; si.mode = "captions"; si.channel = "cap:" + std::to_string(gCapCtx.rx);
        si.domain = "general"; si.rejectMusic = true;
    }
    const int64_t now = wallMs();
    tk->onAudio(n, rate, now);
    gCap.feed(si, mono, n, rate, now);
}

// the aircraft on the screens, by the keys a spoken callsign or registration gives
void refreshAircraft() {
    std::map<std::string, std::string> m;
    for (App* r : receiversAll()) {
        if (!r) continue;
        for (const auto& ac : r->rx.adsb.aircraft) {
            if (ac.icao >> 24) continue;
            char hex[8];
            snprintf(hex, sizeof hex, "%06X", ac.icao & 0xFFFFFFu);
            if (!ac.callsign.empty()) m[dect2::acarsNormFlight(ac.callsign)] = hex;
            const std::string reg = adsbui::registrationOf(ac.icao);
            if (!reg.empty()) m[dect2::acarsNormReg(reg)] = hex;
        }
    }
    std::lock_guard<std::mutex> lk(gMu);
    gAircraft.swap(m);
}

// ---------------------------------------------------------------- telling the engine what is on the air
//
// Once a second, publish the names that are actually in the air into the context store (dect2/transcribe/context.h), which the transcriber
// turns into the speech engine's prompt. An engine that has been told "Speedbird 472" is in range stops inventing "speed bird for seven two".
// Only names, never anything heard: this reads the aircraft table and the airport database, nothing else.
void refreshContext() {
    const dect2::TranscribeConfig tc = gTr.config();
    if (!tc.biasFromContext) {
        dect2::ContextStore::global().clearScope("aviation");
        return;
    }
    const int64_t nowMs = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
    std::vector<dect2::ContextTerm> terms;
    for (App* r : receiversAll()) {
        if (!r) continue;
        for (const auto& ac : r->rx.adsb.aircraft) {
            if (ac.icao >> 24) continue;
            // Weight by how close it is: the aircraft being talked to is far more likely to be the one overhead than one at the edge.
            float w = 1.f;
            if (ac.hasRange && ac.distNm > 0) w = (float)(1.0 + 2.0 / (1.0 + ac.distNm / 25.0));
            const int64_t seen = nowMs - (int64_t)(ac.ageSec > 0 ? ac.ageSec * 1000 : 0);
            if (!ac.callsign.empty()) {
                const std::string spoken = dect2::spokenFlight(dect2::acarsNormFlight(ac.callsign));
                if (!spoken.empty()) terms.push_back({spoken, w, seen});
            }
            // The registration is what a light aircraft is called on the radio, and it is the rarer string of the two.
            const std::string reg = adsbui::registrationOf(ac.icao);
            if (!reg.empty()) terms.push_back({reg, w * 0.9f, seen});
        }
    }
    // The runways of the airports around here: "runway two five left" is said constantly and mis-heard easily.
    double lat = 0, lon = 0;
    receiverLocation(lat, lon);
    if (lat != 0 || lon != 0) {
        const dect2::AirportDb& db = dect2::AirportDb::builtin();
        const std::vector<dect2::AirportHit> near = db.nearby(lat, lon, 60, 6, true);
        for (const dect2::AirportHit& h : near) {
            const dect2::Airport& ap = db.all()[h.index];
            if (!ap.name.empty()) terms.push_back({ap.name, 0.7f, nowMs});
            for (const dect2::Runway& rw : ap.runways) {
                for (const dect2::RunwayEnd* e : {&rw.le, &rw.he}) {
                    const std::string spoken = dect2::spokenRunway(e->ident);
                    // A nearer airport's runways are worth more, but all of them beat nothing.
                    if (!spoken.empty()) terms.push_back({spoken, h.km < 15 ? 1.1f : 0.6f, nowMs});
                }
            }
        }
    }
    dect2::ContextStore::global().setTerms("aviation", std::move(terms));
}

void startLoad(const std::string& file) {
    if (gLoadTh.joinable()) gLoadTh.join();
    gLoadState = 1;
    gLoadingFile = file;
    const std::string path = modelsDir() + "/" + file;
    const int workers = 1;
    gLoadTh = dect2::namedThread("onair-asrload", [path, workers] {
        std::string err;
        auto b = dect2::makeWhisperBackend(path, workers, &err);
        std::lock_guard<std::mutex> lk(gMu);
        if (b) { gPending = b; gLoadErr.clear(); gLoadState = 2; }
        else { gLoadErr = err; gLoadState = 3; }
    });
}

void ensureBackend() {
    const std::string want = ((C.enabled || C.cap) && modelPresent(C.model)) ? modelFile(C.model) : "";
    if (gLoadState.load() == 2) {
        std::lock_guard<std::mutex> lk(gMu);
        if (gPending) {
            gBackend = gPending; gPending.reset();
            gBackendEnglishOnly = gBackend->englishOnly();
            gLoadedFile = gLoadingFile;
            gTr.setBackend(gBackend);
            gCap.setBackend(gBackend);
        }
    }
    if (want.empty()) {
        if (gBackend) { gTr.setBackend(nullptr); gCap.setBackend(nullptr); gBackend.reset(); gLoadedFile.clear(); }
        if (!C.enabled && !C.cap && gLoadState.load() == 3) gLoadState = 0;
        return;
    }
    if (gLoadState.load() == 1) return;
    if (want != gLoadedFile && !(gLoadState.load() == 3 && want == gLoadingFile)) startLoad(want);
}

bool backendReady() { return gBackend != nullptr; }

}  // namespace

bool& windowOpen() { return gWindowOpen; }

bool indicator(std::string& text) {
    if (!backendReady()) return false;
    if (!C.enabled) {
        if (!C.cap) return false;
        text = tr("Generating captions from the sound");
        return true;
    }
    std::string what;
    if (C.airband) what = "airband";
    if (C.marine) what += (what.empty() ? "" : ", ") + std::string("marine");
    if (C.other) what += (what.empty() ? "" : ", ") + std::string("other modes");
    if (what.empty()) return false;
    char b[160];
    snprintf(b, sizeof b, tr("Transcribing speech: %s"), what.c_str());
    text = b;
    return true;
}

void tick(App& first) {
    (void)first;
    load();
    if (!gStarted) {
        gStarted = true;
        gStore.setLimits(20000, C.days);
        std::string err;
        gStore.attachFile(plat::dataDir() + "/transcripts/transcripts.jsonl", &err);
        gTr.setResolver([](const dect2::CallsignHit& h) {
            std::lock_guard<std::mutex> lk(gMu);
            const auto it = gAircraft.find(h.key);
            return it == gAircraft.end() ? std::string() : it->second;
        });
        gCap.setOnEntry([](const dect2::TranscriptEntry& e) {
            int rx = 0;
            if (e.channel.compare(0, 4, "cap:") != 0) return;
            rx = atoi(e.channel.c_str() + 4);
            std::lock_guard<std::mutex> lk(gMu);
            const auto it = gTracks.find(rx);
            if (it != gTracks.end() && it->second) it->second->add(e);
        });
        applyConfig();
    }
    static double last = 0;
    const double now = ImGui::GetTime();
    if (now - last < 1.0) return;
    last = now;
    static int slow = 0;
    if (++slow % 600 == 0) gStore.prune((int64_t)time(nullptr));
    static bool lastEnabled = false;
    static int lastThreads = 0;
    static std::string lastLang;
    static bool lastTr = false, lastCap = false;
    if (lastEnabled != C.enabled || lastThreads != C.threads || lastLang != C.lang || lastTr != C.translate || lastCap != C.cap) { lastCap = C.cap; lastTr = C.translate; lastEnabled = C.enabled; lastThreads = C.threads; lastLang = C.lang; applyConfig(); }
    ensureBackend();
    if (gDl.state() == dect2::ModelDownloader::State::Done) {
        const std::string id = gDl.modelId();
        if (!modelPresent(C.model)) { C.model = id; save(); }
    }

    const bool ready = backendReady() && C.enabled;
    if (ready) { refreshAircraft(); refreshContext(); }
    // airband: a tap on each channel of each running airband receiver
    for (App* r : receiversAll()) {
        if (!r) continue;
        const bool air = r->engine.running() && r->rx.standard == 27;
        const bool want = ready && C.airband && air;
        bool& on = gTapOn[r->rxId];
        if (air) {
            std::vector<dect2::SourceInfo> v;
            for (const auto& c : r->rx.airband.channels) {
                dect2::SourceInfo si;
                si.key = ((int64_t)r->rxId << 40) | (int64_t)std::llround(c.freqHz / 100.0);
                si.freqHz = c.freqHz; si.mode = "airband"; si.domain = "aviation";
                si.channel = c.name + (c.label.empty() ? std::string() : " " + c.label);
                v.push_back(si);
            }
            std::lock_guard<std::mutex> lk(gMu);
            gChanInfo[r->rxId] = std::move(v);
        }
        static std::map<int, double> tapAt;   // installed again now and then: a receiver closed and opened again under the same number has a new engine
        if (want && on && ImGui::GetTime() - tapAt[r->rxId] > 15) on = false;
        if (want && !on) {
            const int id = r->rxId;
            tapAt[id] = ImGui::GetTime();
            r->engine.airband().setChannelTap([id](int chan, const float* x, size_t n) { onAirband(id, chan, x, n); });
            on = true;
        } else if (!want && on) {
            r->engine.airband().setChannelTap(nullptr);
            on = false;
            std::lock_guard<std::mutex> lk(gMu);
            gChanInfo.erase(r->rxId);
        }
    }
    // the sound of the other modes
    bool sink = false;
    {
        std::lock_guard<std::mutex> lk(gMu);
        // captions: the receiver being heard, when its mode is ticked
        const CapCtx old = gCapCtx;
        gCapCtx.valid = false;
        if (C.cap && backendReady()) if (App* h = receiverHeard()) if (h->engine.running() && C.capModes.count(h->rx.standard) && dect2::captionStandardEligible(h->rx.standard)) {
            gCapCtx.valid = true; gCapCtx.rx = h->rxId; gCapCtx.std = h->rx.standard; gCapCtx.freqHz = h->freqMhz * 1e6;
        }
        if (old.valid && (!gCapCtx.valid || old.rx != gCapCtx.rx || old.std != gCapCtx.std || std::fabs(old.freqHz - gCapCtx.freqHz) > 1.0)) {
            // another station: the pending speech goes, the cues start again
            gCap.endSource(0x7100000000000LL + old.rx, wallMs());
            const auto it = gTracks.find(old.rx);
            if (it != gTracks.end() && it->second) it->second->reset();
        }
        gCtx.valid = false;
        if (ready && (C.marine || C.other)) {
            if (App* h = receiverHeard()) if (h->engine.running()) {
                const int std_ = h->rx.standard;
                if (std_ == 16 && C.marine) { gCtx.valid = true; gCtx.mode = "marine"; gCtx.domain = "marine"; gCtx.channel = "Marine VHF"; }
                else if (std_ != 16 && std_ != 27 && C.other) { gCtx.valid = true; gCtx.mode = "audio"; gCtx.domain = "general"; gCtx.channel = "Audio"; }
                gCtx.freqHz = h->freqMhz * 1e6;
            }
        }
        sink = gCtx.valid || gCapCtx.valid;
    }
    if (sink && !gSinkOn) { dect2::AudioTap::instance().setSink(onSound); gSinkOn = true; }
    else if (!sink && gSinkOn) { dect2::AudioTap::instance().setSink(nullptr); gSinkOn = false; gTr.endSource(0x7000000000000LL, wallMs()); }
}

void shutdown() {
    if (gSinkOn) { dect2::AudioTap::instance().setSink(nullptr); gSinkOn = false; }
    for (App* r : receiversAll()) if (r && gTapOn[r->rxId]) { r->engine.airband().setChannelTap(nullptr); gTapOn[r->rxId] = false; }
    gTr.configure([&] { auto c = gTr.config(); c.enabled = false; return c; }());
    gTr.setBackend(nullptr);
    gCap.configure([&] { auto c = gCap.config(); c.enabled = false; return c; }());
    gCap.setBackend(nullptr);
    gBackend.reset();
    if (gLoadTh.joinable()) gLoadTh.join();
    gDl.cancel();
}

// ---------------------------------------------------------------- the lists

namespace {

std::string tstr(int64_t wall) { return acarslog::timeText(wall); }

std::string entryLine(const dect2::TranscriptEntry& e) {
    std::string s = e.channel.empty() ? std::string() : e.channel + ": ";
    s += e.text;
    if (!e.translation.empty()) s += "\n[EN] " + e.translation;
    return s;
}

std::vector<dect2::TranscriptEntry> voiceFor(uint32_t icao, const std::string& callsign, const std::string& reg, size_t limit) {
    std::vector<dect2::TranscriptEntry> v = gStore.forAircraft(callsign, reg, limit);
    if (!(icao >> 24) && icao) {
        char hex[8];
        snprintf(hex, sizeof hex, "%06X", icao & 0xFFFFFFu);
        dect2::TranscriptFilter f;
        f.aircraft = hex;
        for (auto& e : gStore.find(f, limit)) {
            if (e.aircraft != hex) continue;
            bool dup = false;
            for (const auto& x : v) dup |= x.id == e.id;
            if (!dup) v.push_back(e);
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.wall != b.wall ? a.wall > b.wall : a.id > b.id; });
        if (limit && v.size() > limit) v.resize(limit);
    }
    return v;
}

}  // namespace

size_t voiceCount(uint32_t icao, const std::string& callsign, const std::string& reg) { return voiceFor(icao, callsign, reg, 50).size(); }

std::vector<dect2::AcarsLogEntry> voiceForTrack(const std::string& id, const std::string& label, int holders) {
    uint32_t icao = 0;
    bool hex = id.size() == 6;
    for (unsigned char c : id) if (!isxdigit(c)) hex = false;
    std::string reg;
    if (hex) { icao = (uint32_t)strtoul(id.c_str(), nullptr, 16); reg = adsbui::registrationOf(icao); }
    else reg = id;
    std::vector<dect2::AcarsLogEntry> out;
    for (const auto& e : voiceFor(icao, holders == 1 ? label : std::string(), reg, 20)) {
        if (e.mode != "airband" || e.confidence < 0.5f || e.text.size() < 8) continue;
        dect2::AcarsLogEntry m;
        m.wall = e.wall; m.mode = "VOICE"; m.freqMHz = e.freqHz / 1e6; m.label = ""; m.cat = dect2::AcarsCat::Free;
        m.text = e.text; m.flight = e.callsign; m.downlink = true;
        out.push_back(std::move(m));
    }
    return out;
}

void voiceSection(const App& a, uint32_t icao, const std::string& callsign, const std::string& reg) {
    const std::vector<dect2::TranscriptEntry> v = voiceFor(icao, callsign, reg, 30);
    ImGui::Dummy(ImVec2(0, 4 * gUi));
    ImGui::Separator();
    ImGui::Text("%s%s", tr("Voice (radio calls)"), v.empty() ? "" : (" (" + std::to_string(v.size()) + ")").c_str());
    if (!v.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy##voice"))) {
            std::string t;
            for (const auto& e : v) t += tstr(e.wall) + "  " + entryLine(e) + "\n";
            ImGui::SetClipboardText(t.c_str());
        }
    }
    if (v.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", C.enabled ? tr("No radio call with this aircraft's callsign heard yet") : tr("Live transcription is off (View > Transcripts). Airband calls would be listed here by callsign."));
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild("##voicelog", ImVec2(0, 180 * gUi), true);
    ImGui::PushFont(a.mono, 0);
    for (const auto& e : v) {
        ImGui::TextDisabled("%s  %s%s%s", tstr(e.wall).c_str(), e.channel.c_str(), e.speaker.empty() ? "" : "  ", e.speaker.c_str());
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str());
        if (!e.translation.empty()) { ImGui::TextDisabled("EN"); ImGui::SameLine(); ImGui::TextUnformatted(e.translation.c_str()); }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 2 * gUi));
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the live captions of the airband screen

void captions(App& a) {
    load();
    ImGui::PushID("##captions");
    std::string ind;
    if (!C.enabled || !backendReady()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", C.enabled ? tr("Live captions: loading the speech model ...") : tr("Live captions: off."));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Set up transcripts"))) gWindowOpen = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Turns what is said on the airband channels into text as it is heard. It needs a speech model that you download or put in the models folder."));
        ImGui::Spacing();
        ImGui::PopID();
        return;
    }
    const dect2::TranscriberStats st = gTr.stats();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::warnAmber(), "%s", tr("Live captions"));
    ImGui::SameLine();
    if (st.busy || st.queued) ImGui::TextDisabled("%s", tr("working ..."));
    else if (gTr.active()) ImGui::TextDisabled("%s", tr("listening ..."));
    else ImGui::TextDisabled("%s", " ");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Transcripts"))) gWindowOpen = true;
    if (!C.airband) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", tr("Airband is off in the transcript settings")); }
    dect2::TranscriptFilter f;
    f.mode = "airband";
    const auto v = gStore.find(f, 3);
    ImGui::BeginChild("##capbox", ImVec2(0, 78 * gUi), true);
    if (v.empty()) ImGui::TextDisabled("%s", tr("Nothing heard yet."));
    ImGui::PushTextWrapPos(0);
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        ImGui::TextDisabled("%s  %s", tstr(it->wall).c_str(), it->channel.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(it->text.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    (void)a;
    ImGui::PopID();
}

// ---------------------------------------------------------------- the Transcripts window

namespace {

struct Ui {
    char query[96] = "", aircraft[32] = "", airport[16] = "";
    int freqSel = 0;                           // 0 all, else index into freqs
    std::vector<double> freqs;
    std::vector<dect2::TranscriptEntry> rows;
    uint64_t ver = ~0ull;
    std::string key;
    double at = -10;
    size_t total = 0;
    int selected = -1;
    bool askDelete = false;
    const dect2::SpeechModel* askModel = nullptr;
    std::string note;
    bool settings = false;
};
Ui U;

void refreshRows() {
    dect2::TranscriptFilter f;
    f.query = U.query; f.aircraft = U.aircraft; f.airport = U.airport;
    if (U.freqSel > 0 && (size_t)U.freqSel <= U.freqs.size()) f.freqHz = U.freqs[(size_t)U.freqSel - 1];
    char k[400];
    snprintf(k, sizeof k, "%s|%s|%s|%d", U.query, U.aircraft, U.airport, U.freqSel);
    const double now = ImGui::GetTime();
    if (gStore.version() == U.ver && U.key == k && now - U.at < 5) return;
    U.ver = gStore.version(); U.key = k; U.at = now;
    U.rows = gStore.find(f);
    U.total = gStore.size();
    std::map<long long, int> fr;
    for (const auto& e : gStore.find({})) if (e.freqHz > 0) fr[(long long)std::llround(e.freqHz / 1000.0)]++;
    const double cur = (U.freqSel > 0 && (size_t)U.freqSel <= U.freqs.size()) ? U.freqs[(size_t)U.freqSel - 1] : 0;
    U.freqs.clear();
    for (auto& kv : fr) U.freqs.push_back((double)kv.first * 1000.0);
    U.freqSel = 0;
    if (cur > 0) for (size_t i = 0; i < U.freqs.size(); i++) if (std::fabs(U.freqs[i] - cur) < 1500) U.freqSel = (int)i + 1;
}

void exportRows(const char* ext) {
    std::string def = std::string("onair_transcripts.") + ext;
    const std::string path = plat::saveFileDialog(def.c_str());
    if (path.empty()) return;
    std::string data;
    if (!strcmp(ext, "txt")) data = dect2::transcriptsTxt(U.rows);
    else if (!strcmp(ext, "srt")) data = dect2::transcriptsSrt(U.rows);
    else if (!strcmp(ext, "json")) data = dect2::transcriptsJson(U.rows);
    else data = dect2::transcriptsCsv(U.rows);
    if (!strcmp(ext, "csv")) data = "\xEF\xBB\xBF" + data;   // Excel reads it as UTF-8 then
    FILE* f = dect2::u8fopen(path, "wb");
    char nb[1024];
    if (!f) { snprintf(nb, sizeof nb, tr("Cannot write %s"), path.c_str()); U.note = nb; return; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    snprintf(nb, sizeof nb, tr("Saved %s"), path.c_str());
    U.note = nb;
}

const char* kLangs[][2] = {{"en", "English"}, {"auto", "Auto-detect (needs a multilingual model)"}, {"de", "Deutsch"}, {"fr", "Français"}, {"es", "Español"},
                           {"it", "Italiano"}, {"nl", "Nederlands"}, {"pl", "Polski"}, {"pt", "Português"}, {"lt", "Lietuvių"}, {"ru", "Русский"}, {"hu", "Magyar"},
                           {"tr", "Türkçe"}, {"ar", "العربية"}, {"zh", "中文"}, {"ja", "日本語"}};

void settingsPanel() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::warnAmber(), "%s", tr("Privacy: what is said on public aviation radio is public, but the speech engine runs here on your computer and the text is kept in your data folder. Do not use it on private conversations: marine, amateur, CB and every other mode are off until you choose them, and a visible line shows whenever transcription is on."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Transcribe speech"), &C.enabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Master switch. While a transmission is going on the audio is cut out, sent to the speech engine and turned into text. Nothing runs while the channels are quiet."));
    ImGui::BeginDisabled(!C.enabled);
    ch |= ImGui::Checkbox(tr("Airband channels"), &C.airband);
    ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("Marine VHF"), &C.marine);
    ImGui::SameLine();
    ch |= ImGui::Checkbox(tr("Any other mode that plays sound (amateur, CB, FM ...)"), &C.other);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Takes the sound you hear. Music and private conversations are not meant for this: leave it off unless you need it."));
    ImGui::EndDisabled();
    if (ch) { save(); applyConfig(); }

    // what the engine is given, which is not what you hear
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("What the speech engine is given"));
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("A radio channel carries things that are not speech and that a speech engine will still try to turn into words. This cleans up the copy the engine reads. It never changes what you hear or what is recorded."));
    ImGui::PopTextWrapPos();
    bool fc = false;
    fc |= ImGui::Checkbox(tr("Condition the audio for the engine"), &C.front);
    ImGui::BeginDisabled(!C.front);
    ImGui::Indent();
    fc |= ImGui::Checkbox(tr("Keep 300 - 3400 Hz"), &C.frontBand);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Takes out CTCSS tones (67 - 254 Hz), DCS turn-off bitstreams and the hiss above the voice band."));
    ImGui::SameLine();
    fc |= ImGui::Checkbox(tr("Mute signalling bursts"), &C.frontBurst);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("MDC-1200 and single-tone key-ups, muted before the speech detector sees them so they cannot start a transmission or be read as syllables."));
    ImGui::Unindent();
    ImGui::EndDisabled();
    fc |= ImGui::Checkbox(tr("Tell the engine what is on the air"), &C.bias);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The call signs of the aircraft in range, the runways of the airports around you and the talkgroup names of a trunked system, offered to the engine as words to expect. Only names, never anything heard."));
    if (fc) { save(); applyConfig(); }
    // what is in the prompt right now, so the setting is not a matter of faith
    if (C.bias) {
        const size_t nTerms = dect2::ContextStore::global().termCount();
        if (nTerms == 0) ImGui::TextDisabled("%s", tr("Nothing on the air to tell it about yet."));
        else {
            dect2::SourceInfo probe;
            probe.domain = "aviation";
            const int64_t nowMs = (int64_t)time(nullptr) * 1000;
            const std::vector<dect2::ContextTerm> v =
                dect2::ContextStore::global().termsFor(probe, 600, nowMs, 600 * 1000);
            std::string line;
            for (size_t i = 0; i < v.size() && i < 12; i++) { if (i) line += ", "; line += v[i].spoken; }
            if (v.size() > 12) line += ", ...";
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s %zu: %s", tr("Expecting, of"), nTerms, line.empty() ? tr("(none for aviation)") : line.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    // the model
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Speech model"));
    const std::string dir = modelsDir();
    const double now = ImGui::GetTime();
    if (now - gUserFilesAt > 3) { gUserFiles = dect2::userSpeechModels(dir); gUserFilesAt = now; }
    std::string cur = tr("none");
    if (modelPresent(C.model)) cur = C.model.compare(0, 5, "file:") == 0 ? C.model.substr(5) : C.model;
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo("##txmodel", cur.c_str())) {
        for (const auto& m : dect2::speechModels()) {
            if (!dect2::speechModelInstalled(dir, m)) continue;
            if (ImGui::Selectable(m.id, C.model == m.id)) { C.model = m.id; save(); }
        }
        for (const auto& f : gUserFiles) if (ImGui::Selectable(f.c_str(), C.model == "file:" + f)) { C.model = "file:" + f; save(); }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Open models folder"))) {
        std::error_code ec;
        std::filesystem::create_directories(dect2::u8file(dir), ec);
        dect2::openUrl(dir);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", dir.c_str(), tr("Put a whisper.cpp model file (ggml-*.bin) here to use it. The folder is made if it does not exist."));
    const int ls = gLoadState.load();
    if (C.enabled && !modelPresent(C.model)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("No speech model yet. Download one below, or put a model file in the models folder."));
        ImGui::PopTextWrapPos();
    } else if (C.enabled && ls == 1) ImGui::TextDisabled("%s", tr("Loading the speech model ..."));
    else if (C.enabled && ls == 3) { ImGui::PushTextWrapPos(0); { std::lock_guard<std::mutex> lk(gMu); ImGui::TextColored(pal::badRed(), "%s", gLoadErr.c_str()); } ImGui::PopTextWrapPos(); }
    else if (C.enabled && backendReady()) ImGui::TextColored(pal::okGreen(), "%s", tr("Speech model loaded."));
    if (!dect2::whisperAvailable()) { ImGui::TextColored(pal::badRed(), "%s", tr("This build has no speech engine.")); }

    // download, on an explicit click
    const auto st = gDl.state();
    if (st == dect2::ModelDownloader::State::Downloading || st == dect2::ModelDownloader::State::Verifying) {
        const double tot = (double)std::max<uint64_t>(1, gDl.totalBytes());
        char ov[96];
        snprintf(ov, sizeof ov, "%s %.0f / %.0f MB", st == dect2::ModelDownloader::State::Verifying ? tr("checking") : gDl.modelId().c_str(), (double)gDl.doneBytes() / 1048576.0, tot / 1048576.0);
        ImGui::ProgressBar((float)(gDl.doneBytes() / tot), ImVec2(320 * gUi, 0), ov);
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##txdl"))) gDl.cancel();
    } else {
        if (st == dect2::ModelDownloader::State::Failed) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(pal::badRed(), "%s", gDl.error().c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextDisabled("%s", tr("Download speech model:"));
        for (const char* id : {"tiny.en", "base.en", "small.en"}) {
            const dect2::SpeechModel* m = dect2::findSpeechModel(id);
            if (!m) continue;
            ImGui::SameLine();
            char b[96];
            snprintf(b, sizeof b, tr("%s %d MB"), m->id, m->mib);
            const bool have = dect2::speechModelInstalled(dir, *m);
            ImGui::BeginDisabled(have);
            if (ImGui::Button((std::string(b) + "##dl" + id).c_str())) U.askModel = m;
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", have ? tr("Already downloaded") : tr("English only. tiny is the fastest (real time on a laptop), small hears more words and needs a stronger computer."));
        }
        if (ImGui::TreeNode(tr("Other languages (multilingual models)"))) {
            for (const char* id : {"tiny", "base", "small"}) {
                const dect2::SpeechModel* m = dect2::findSpeechModel(id);
                if (!m) continue;
                char b[96];
                snprintf(b, sizeof b, tr("%s %d MB"), m->id, m->mib);
                const bool have = dect2::speechModelInstalled(dir, *m);
                ImGui::BeginDisabled(have);
                if (ImGui::Button((std::string(b) + "##dl" + id).c_str())) U.askModel = m;
                ImGui::EndDisabled();
                ImGui::SameLine();
            }
            ImGui::NewLine();
            ImGui::TreePop();
        }
    }
    // language
    ImGui::SetNextItemWidth(260 * gUi);
    std::string langName = C.lang;
    for (auto& l : kLangs) if (C.lang == l[0]) langName = l[1];
    if (ImGui::BeginCombo(tr("Language"), langName.c_str())) {
        for (auto& l : kLangs) if (ImGui::Selectable(l[1], C.lang == l[0])) { C.lang = l[0]; save(); applyConfig(); }
        ImGui::EndCombo();
    }
    if (C.lang != "en" && backendReady() && gBackendEnglishOnly) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("The loaded model is English only: other languages need a multilingual model (tiny, base or small without .en)."));
        ImGui::PopTextWrapPos();
    }
    if (ImGui::Checkbox(tr("Translate to English"), &C.translate)) { save(); applyConfig(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Speech in another language is also translated to English, offline, next to the original. Needs a multilingual model and costs about 1.5 times more processor time; it is skipped when the engine cannot keep up. Nothing is sent anywhere. Translation into other languages is not available."));
    if (C.translate && backendReady() && gBackendEnglishOnly) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("The loaded model is English only: translation needs a multilingual model."));
        ImGui::PopTextWrapPos();
    }
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderInt(tr("CPU threads"), &C.threads, 1, 8)) { save(); applyConfig(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How many processor threads the speech engine may use for one transmission. Fewer threads use less battery and take longer."));
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderInt(tr("Keep transcripts (days)"), &C.days, 1, 90)) { save(); gStore.setLimits(20000, C.days); }
    const dect2::TranscriberStats ts = gTr.stats();
    ImGui::TextDisabled(tr("Heard %llu, kept %llu, left out %llu (silence, noise, repeats), skipped %llu (too slow), speed %.2f x real time"), (unsigned long long)ts.segments,
                        (unsigned long long)ts.kept, (unsigned long long)ts.filtered, (unsigned long long)ts.dropped, ts.avgRtf);
}

void modelPopups() {
    if (U.askModel) { ImGui::OpenPopup("##txdlask"); }
    if (ImGui::BeginPopupModal("##txdlask", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (U.askModel) {
            ImGui::PushTextWrapPos(480 * gUi);
            ImGui::Text(tr("Download the speech model %s (%d MB)?"), U.askModel->id, U.askModel->mib);
            ImGui::TextDisabled("%s", dect2::speechModelUrl(*U.askModel).c_str());
            ImGui::TextUnformatted(tr("Eye of ZorCom asks the model repository (huggingface.co) for this one file and sends nothing else. The file is checked against a fixed SHA-256 checksum and thrown away if it does not match. The model is MIT licensed."));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button(tr("Download"), ImVec2(140 * gUi, 0))) {
                gDl.start(*U.askModel, modelsDir());
                U.askModel = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(tr("Cancel##txask"), ImVec2(100 * gUi, 0))) { U.askModel = nullptr; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (U.askDelete) { ImGui::OpenPopup("##txdel"); U.askDelete = false; }
    if (ImGui::BeginPopupModal("##txdel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("Delete all saved transcripts? This cannot be undone."));
        if (ImGui::Button(tr("Delete all"), ImVec2(140 * gUi, 0))) { gStore.clear(); U.ver = ~0ull; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##txdelc"), ImVec2(100 * gUi, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace

void windowPanel(App& a) {
    load();
    if (!gWindowOpen) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1000 * gUi, vp->WorkSize.x * 0.95f), std::min(660 * gUi, vp->WorkSize.y * 0.92f)), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Transcripts"), &gWindowOpen)) { ImGui::End(); return; }
    std::string ind;
    if (indicator(ind)) {
        ImGui::TextColored(pal::badRed(), "%s", "\xE2\x97\x8F");
        ImGui::SameLine();
        ImGui::TextColored(pal::warnAmber(), "%s", ind.c_str());
    } else ImGui::TextDisabled("%s", tr("Transcription is off."));
    ImGui::SameLine();
    if (ImGui::SmallButton(U.settings ? tr("Hide settings") : tr("Settings and speech model"))) U.settings = !U.settings;
    if (U.settings || !C.enabled || !backendReady()) {
        ImGui::BeginChild("##txset", ImVec2(0, (U.settings ? 460.f : 150.f) * gUi), true);
        settingsPanel();
        captionsSettings(a);
        ImGui::EndChild();
    }
    refreshRows();
    // filters
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##txq", tr("Search the text ..."), U.query, sizeof U.query);
    ImGui::SameLine();
    {
        std::string cur = U.freqSel > 0 && (size_t)U.freqSel <= U.freqs.size() ? std::to_string(U.freqs[(size_t)U.freqSel - 1] / 1e6).substr(0, 7) + " MHz" : std::string(tr("All frequencies"));
        ImGui::SetNextItemWidth(150 * gUi);
        if (ImGui::BeginCombo("##txf", cur.c_str())) {
            if (ImGui::Selectable(tr("All frequencies"), U.freqSel == 0)) U.freqSel = 0;
            for (size_t i = 0; i < U.freqs.size(); i++) {
                char b[32];
                snprintf(b, sizeof b, "%.3f MHz", U.freqs[i] / 1e6);
                if (ImGui::Selectable(b, U.freqSel == (int)i + 1)) U.freqSel = (int)i + 1;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputTextWithHint("##txa", tr("Aircraft"), U.aircraft, sizeof U.aircraft);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A callsign (DLH123), a registration (N123AB) or an ICAO address (3C6444)"));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    ImGui::InputTextWithHint("##txap", tr("Airport"), U.airport, sizeof U.airport);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("An airport code or name found in the channel name or the text (EYVI, Tower)"));
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%zu of %zu"), U.rows.size(), U.total);
    // actions
    if (ImGui::SmallButton(tr("Copy all shown"))) ImGui::SetClipboardText(dect2::transcriptsTxt(U.rows).c_str());
    for (const char* ext : {"txt", "srt", "json", "csv"}) {
        ImGui::SameLine();
        char b[32];
        snprintf(b, sizeof b, tr("Export %s##txe"), ext);
        if (ImGui::SmallButton(b)) exportRows(ext);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Delete all ..."))) U.askDelete = true;
    if (!U.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", U.note.c_str()); }
    // the list
    if (ImGui::BeginTable("##txlist", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time (UTC)"), ImGuiTableColumnFlags_WidthFixed, 120 * gUi);
        ImGui::TableSetupColumn(tr("Channel"), ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
        ImGui::TableSetupColumn(tr("Text"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Aircraft"), ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
        ImGui::TableSetupColumn(tr("Sure"), ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
        ImGui::TableHeadersRow();
        const size_t shown = std::min<size_t>(U.rows.size(), 600);
        for (size_t i = 0; i < shown; i++) {
            const auto& e = U.rows[i];
            ImGui::PushID((int)e.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            std::tm tm{};
            const std::time_t t = (std::time_t)e.wall;
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            char d[32];
            std::strftime(d, sizeof d, "%m-%d %H:%M:%S", &tm);
            ImGui::PushFont(a.mono, 0);
            ImGui::TextUnformatted(d);
            ImGui::PopFont();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(e.channel.c_str());
            if (!e.speaker.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", tr("Speaker (a guess from the wording)"), e.speaker.c_str());
            ImGui::TableNextColumn();
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(e.text.c_str());
            if (!e.translation.empty()) {
                ImGui::TextDisabled("%s > EN", e.lang.empty() ? "?" : e.lang.c_str());
                ImGui::SameLine();
                ImGui::TextUnformatted(e.translation.c_str());
            }
            ImGui::PopTextWrapPos();
            if (ImGui::IsItemClicked()) ImGui::SetClipboardText(entryLine(e).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click to copy this line"));
            ImGui::TableNextColumn();
            const std::string ac = !e.callsign.empty() ? e.callsign : e.reg;
            if (!ac.empty()) ImGui::TextUnformatted(ac.c_str()); else ImGui::TextDisabled("-");
            if (!e.aircraft.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", tr("Aircraft on the screen"), e.aircraft.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%.0f%%", e.confidence * 100.0);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    modelPopups();
    ImGui::End();
}


// ---------------------------------------------------------------- auto subtitles

namespace {

bool isTvStandard(int s) { return s == 0 || s == 1 || s == 3 || s == 4 || s == 6 || s == 7 || s == 8 || s == 40 || s == 47; }

dect2::CaptionTrack* trackOf(int rx) {   // gMu held by the caller
    const auto it = gTracks.find(rx);
    return it == gTracks.end() ? nullptr : it->second.get();
}

std::string capLine(const dect2::CaptionCue& c) {
    if (C.capShow == 1 && !c.translation.empty()) return c.translation;
    if (C.capShow == 2 && !c.translation.empty()) return c.text + "\n" + c.translation;
    return c.text;
}

bool writeText(const std::string& path, const std::string& body) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dect2::u8file(path)).parent_path(), ec);
    std::ofstream f(dect2::u8file(path), std::ios::binary);
    if (!f) return false;
    f << body;
    return (bool)f;
}

// cue lines wrapped to a width, drawn in a box on the draw list; bottom (or top) of the area
void drawCues(ImDrawList* dl, ImVec2 a0, ImVec2 asz, const std::vector<dect2::CaptionCue>& cues, bool forceTop = false) {
    dect2::CaptionStyle st = dect2::CaptionStyle{C.capSize, C.capBg, C.capTop, C.capLines}.clamped();
    if (forceTop) st.top = true;   // next to the station's own subtitles, which sit at the bottom
    const float fs = ImGui::GetFontSize() * (float)st.sizePct / 100.f * std::max(1.f, asz.y / (260.f * gUi) * 0.6f);
    const float maxW = asz.x * 0.9f;
    ImFont* font = ImGui::GetFont();
    std::vector<std::string> lines;
    for (const auto& c : cues) lines.push_back(capLine(c));
    std::vector<ImVec2> sz;
    float h = 0, w = 0;
    for (const auto& l : lines) {
        const ImVec2 t = font->CalcTextSizeA(fs, FLT_MAX, maxW, l.c_str());
        sz.push_back(t); h += t.y + 4 * gUi; w = std::max(w, t.x);
    }
    if (lines.empty()) return;
    const float pad = 8 * gUi;
    float y = st.top ? a0.y + asz.y * 0.05f : a0.y + asz.y * 0.95f - h - pad;
    const float x0 = a0.x + (asz.x - w) * 0.5f;
    dl->AddRectFilled(ImVec2(x0 - pad, y - pad * 0.5f), ImVec2(x0 + w + pad, y + h + pad * 0.5f), IM_COL32(0, 0, 0, (int)(255 * st.bgAlpha / 100.f)), 6 * gUi);
    for (size_t i = 0; i < lines.size(); i++) {
        const float x = a0.x + (asz.x - sz[i].x) * 0.5f;
        dl->AddText(font, fs, ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, 200), lines[i].c_str(), nullptr, maxW);
        dl->AddText(font, fs, ImVec2(x, y), IM_COL32(255, 255, 255, 255), lines[i].c_str(), nullptr, maxW);
        y += sz[i].y + 4 * gUi;
    }
}

std::vector<dect2::CaptionCue> visibleFor(int rx) {
    std::lock_guard<std::mutex> lk(gMu);
    dect2::CaptionTrack* t = trackOf(rx);
    return t ? t->visible((size_t)std::max(1, std::min(4, C.capLines))) : std::vector<dect2::CaptionCue>();
}

std::string exportNow(App& a, int rx, bool vtt) {
    dect2::CaptionTrack* t;
    std::vector<dect2::CaptionCue> none;
    std::string body;
    {
        std::lock_guard<std::mutex> lk(gMu);
        t = trackOf(rx);
        if (!t || t->size() == 0) return "";
        body = vtt ? t->vtt(0, 0, C.capShow) : t->srt(0, 0, C.capShow);
    }
    char nm[128];
    const time_t tt = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    snprintf(nm, sizeof nm, "/captions/onair_captions_%.3fMHz_%s.%s", a.freqMhz, ts, vtt ? "vtt" : "srt");
    const std::string path = plat::dataDir() + nm;
    return writeText(path, body) ? path : "";
}

}  // namespace

bool captionsActive() { load(); return C.cap && backendReady(); }
bool captionsWanted(int standard) { load(); return C.cap && C.capModes.count(standard) && dect2::captionStandardEligible(standard); }
bool captionsIsTv(int standard) { return isTvStandard(standard); }
bool captionsShowBroadcast() { load(); return dect2::showBroadcastSubs((dect2::CaptionSource)C.capSource); }

void captionsOverlay(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 size, bool broadcastPresent) {
    load();
    if (!captionsActive() || !captionsWanted(a.rx.standard)) return;
    if (!dect2::showGeneratedSubs((dect2::CaptionSource)C.capSource, broadcastPresent)) return;
    drawCues(dl, p0, size, visibleFor(a.rxId), C.capSource == 2 && broadcastPresent);
}

void captionsWindow(App& a) {
    (void)a;
    load();
    if (!captionsActive()) return;
    App* h = receiverHeard();
    if (!h || !h->engine.running()) return;
    const int std_ = h->rx.standard;
    if (!captionsWanted(std_) || isTvStandard(std_)) return;
    const dect2::CaptionStyle st = dect2::CaptionStyle{C.capSize, C.capBg, C.capTop, C.capLines}.clamped();
    bool open = true;
    char id[64];
    snprintf(id, sizeof id, "%s###caption%d", tr("Captions"), h->rxId);
    ImGui::SetNextWindowSize(ImVec2(560 * gUi, 110 * gUi), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Pos.x + 80 * gUi, ImGui::GetMainViewport()->Pos.y + ImGui::GetMainViewport()->Size.y - 220 * gUi), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha((float)st.bgAlpha / 100.f);
    if (ImGui::Begin(id, &open, ImGuiWindowFlags_NoCollapse)) {
        const auto cues = visibleFor(h->rxId);
        const dect2::TranscriberStats ts = gCap.stats();
        ImGui::SetWindowFontScale((float)st.sizePct / 100.f);
        ImGui::PushTextWrapPos(0);
        if (cues.empty()) ImGui::TextDisabled("%s", (ts.busy || ts.queued) ? tr("working ...") : tr("listening ..."));
        for (const auto& c : cues) {
            ImGui::TextUnformatted(capLine(c).c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::SetWindowFontScale(1.f);
        ImGui::Separator();
        if (ImGui::SmallButton(tr("Save SRT"))) { const std::string p = exportNow(*h, h->rxId, false); h->engine.log(p.empty() ? "captions: nothing to save yet" : "captions saved to " + p); }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Save VTT"))) { const std::string p = exportNow(*h, h->rxId, true); h->engine.log(p.empty() ? "captions: nothing to save yet" : "captions saved to " + p); }
        ImGui::SameLine();
        ImGui::TextDisabled(ts.avgRtf > 1.0 ? tr("behind real time (%.1f x): the oldest speech is skipped") : tr("speed %.2f x real time"), ts.avgRtf);
        if (ts.musicSkipped) { ImGui::SameLine(); ImGui::TextDisabled(tr("music skipped %llu"), (unsigned long long)ts.musicSkipped); }
    }
    ImGui::End();
    if (!open) { C.capModes.erase(std_); save(); }
}

void captionsControls(App& a) {
    load();
    const int std_ = a.rx.standard;
    if (!dect2::captionStandardEligible(std_)) return;
    flowNext();
    bool on = C.cap && C.capModes.count(std_);
    if (ImGui::Checkbox(tr("auto captions"), &on)) {
        if (on) { C.capModes.insert(std_); C.cap = true; } else C.capModes.erase(std_);
        save(); applyConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Captions made on your computer from the speech you hear, with the offline speech model (View > Transcripts > Settings). Nothing is sent anywhere."));
    if (isTvStandard(std_) && on) {
        flowNext();
        ImGui::SetNextItemWidth(230 * gUi);
        const char* names[] = {tr("Station's own subtitles"), tr("Generated from speech"), tr("Both"), tr("Generated where the station has none")};
        if (ImGui::BeginCombo("##capsrc", names[C.capSource])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(names[i], i == C.capSource)) { C.capSource = i; save(); }
            ImGui::EndCombo();
        }
    }
}

void captionsSettings(App& a) {
    (void)a;
    load();
    ImGui::Spacing();
    if (!ImGui::TreeNode(tr("Auto subtitles (captions)"))) return;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Captions from the speech of the sound you hear: FM and AM broadcast, DAB, TV audio, amateur and digital voice. They are made here with the offline speech model, shown on the receiver, and kept only in memory (and in .srt / .vtt files if you save them). Music is skipped. Off until you turn it on."));
    ImGui::PopTextWrapPos();
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Make captions"), &C.cap);
    ImGui::BeginDisabled(!C.cap);
    ImGui::TextUnformatted(tr("Modes"));
    int col = 0;
    for (int s : dect2::captionStandards()) {
        bool on = C.capModes.count(s) != 0;
        if (ImGui::Checkbox((std::string(dect2::captionStandardName(s)) + "##capm" + std::to_string(s)).c_str(), &on)) { if (on) C.capModes.insert(s); else C.capModes.erase(s); ch = true; }
        if (++col % 3) ImGui::SameLine(); 
    }
    if (col % 3) ImGui::NewLine();
    ImGui::SetNextItemWidth(260 * gUi);
    const char* srcs[] = {tr("Station's own subtitles"), tr("Generated from speech"), tr("Both"), tr("Generated where the station has none")};
    if (ImGui::BeginCombo(tr("TV subtitles"), srcs[C.capSource])) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(srcs[i], i == C.capSource)) { C.capSource = i; ch = true; }
        ImGui::EndCombo();
    }
    const char* shows[] = {tr("As spoken"), tr("English translation"), tr("Both")};
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::BeginCombo(tr("Caption text"), shows[C.capShow])) {
        for (int i = 0; i < 3; i++) if (ImGui::Selectable(shows[i], i == C.capShow)) { C.capShow = i; ch = true; }
        ImGui::EndCombo();
    }
    if (C.capShow && !C.translate) ImGui::TextColored(pal::warnAmber(), "%s", tr("Tick Translate to English above (multilingual model) to get translated captions."));
    ImGui::SetNextItemWidth(160 * gUi);
    ch |= ImGui::SliderInt(tr("Caption size, percent"), &C.capSize, 60, 250);
    ImGui::SetNextItemWidth(160 * gUi);
    ch |= ImGui::SliderInt(tr("Background, percent"), &C.capBg, 0, 100);
    ImGui::SetNextItemWidth(160 * gUi);
    ch |= ImGui::SliderInt(tr("Lines"), &C.capLines, 1, 4);
    ch |= ImGui::Checkbox(tr("At the top of the picture"), &C.capTop);
    ch |= ImGui::Checkbox(tr("Save .srt and .vtt next to audio recordings"), &C.capSave);
    ImGui::EndDisabled();
    if (ch) { C.capSize = dect2::CaptionStyle{C.capSize, C.capBg, C.capTop, C.capLines}.clamped().sizePct; save(); applyConfig(); }
    const dect2::TranscriberStats ts = gCap.stats();
    ImGui::TextDisabled(tr("Captions: %llu pieces, %llu music skipped, %llu skipped (too slow), speed %.2f x real time"), (unsigned long long)ts.segments, (unsigned long long)ts.musicSkipped, (unsigned long long)ts.dropped, ts.avgRtf);
    if (ts.avgRtf > 1.0) ImGui::TextColored(pal::warnAmber(), "%s", tr("The speech engine is slower than the audio: use a smaller model or fewer CPU threads. The oldest speech is skipped, never the newest."));
    ImGui::TreePop();
}

void captionsRecordStart() {
    std::lock_guard<std::mutex> lk(gMu);
    if (!gCapCtx.valid) return;
    if (dect2::CaptionTrack* t = trackOf(gCapCtx.rx)) t->markOrigin();
}

void captionsRecordStop(const std::string& recordingPath) {
    load();
    if (!C.cap || !C.capSave || recordingPath.empty()) return;
    std::string srt, vtt;
    {
        std::lock_guard<std::mutex> lk(gMu);
        dect2::CaptionTrack* t = nullptr;
        if (gCapCtx.valid) t = trackOf(gCapCtx.rx);
        if (!t || t->size() == 0) return;
        const double o = t->origin();
        srt = t->srt(o, 0, C.capShow);
        vtt = t->vtt(o, 0, C.capShow);
    }
    if (srt.empty()) return;
    std::string base = recordingPath;
    const size_t dot = base.find_last_of('.'), sl = base.find_last_of("/\\");
    if (dot != std::string::npos && (sl == std::string::npos || dot > sl)) base.resize(dot);
    writeText(base + ".srt", srt);
    writeText(base + ".vtt", vtt);
}

}  // namespace tx
