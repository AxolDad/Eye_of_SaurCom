// P25 Phase 1 screens: the current call (unit ID, talkgroup, emergency, encryption, voice), the call log (calls, voice grants, data
// packets), the trunking system (site, adjacent sites, channel identifiers), the talkgroups with their activity, the decoded control
// messages, and a "?" about the mbelib library the voice needs (docs/CODECS.md). Receive only; encrypted calls are only reported.
#include "app.h"
#include "dect2/codec/codec_registry.h"
#include "../codec/speech_codec_ui.h"
#include "dect2/p25/p25_imbe.h"
#include "dect2/p25/p25_proto.h"
#include "dect2/p25/p25_rx.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25/trunk_voice.h"
#include "dect2/dmr/dmr_rx.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/ltr/ltr_proto.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/ltr/ltr_tel.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "state/receivers.h"
#include "dect2/engine/device_claims.h"
#include "ui/notices.h"
#include "dect2/transcribe/context.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/common/u8file.h"
#include "platform/platform.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace {

struct State {
    int view = 0;                        // 0 now, 1 calls, 2 trunking, 3 talkgroups, 4 messages, 5 follow
    bool wasRunning = false;
    float hang = 2.0f;
    float pushedVol = -1; bool pushedMute = false;
};
State S;

// ---------------------------------------------------------------- the trunking follower (core/src/p25/trunk_*.cpp, docs/trunking.md)
struct Trunk {
    dect2::trunk::Controller ctl;
    bool on = false, loaded = false, dirty = false;
    struct Seen { uint64_t tsbk = 0, csbk = 0, nxdn = 0, nxdnCac = 0, edacs = 0, ltr = 0, mpt = 0; };
    std::map<int, Seen> seen;            // per receiver (the message sequence numbers are not per receiver): what of its messages was taken
    // the user's channel lists, as typed or pasted ("LCN,frequency" lines; the older "LCN=MHz" pairs read too), saved in the prefs of RX 1
    std::string dmrChannels;             // Tier III (pref "trunkDmrChannels")
    std::string nxdnChannels;            // NXDN Type C (pref "trunkNxdnChannels")
    std::string edacsChannels;           // EDACS LCNs (pref "trunkEdacsLcns")
    std::string ltrChannels;             // LTR repeaters (pref "trunkLtrRepeaters")
    std::string mptChannels;             // MPT 1327 channels that do not follow the formula (pref "trunkMptChannels")
    double mptBaseMhz = 0, mptSpacingKhz = 12.5;   // MPT 1327: channel n is on base + n x spacing (prefs "trunkMptBase", "trunkMptSpacing")
    bool mptBaseAuto = false;            // the base was worked out from the control channel's broadcast (the user did not type one)
    bool edacsNarrow = false;            // EDACS narrowband, 4800 bit/s (pref "trunkEdacsNarrow")
    dect2::trunk::VoiceWatch selfWatch;  // what the control receiver hears of the call it hopped to (analog systems)
    int forcedSel = 0;
    uint64_t recCall = 0;
    std::string status;
    char csvPath[300] = "";
    // voice radios (docs/trunking.md, "Several radios"): other receivers that take the calls outside the capture, so the control radio stays put
    struct VoiceRadio {
        int rxId = 0;
        dect2::trunk::VoiceWatch watch;  // what its receiver heard of the call it was sent to
        double linkedSec = 0;
        bool ran = false;                // its receiver has been running since it was linked
    };
    std::vector<VoiceRadio> voice;       // radio k of the follower is voice[k]
    int ctlRx = 0;                       // the receiver that drives the follower (its control receiver); kept until it stops or leaves trunking
    int linkFamily = -1, linkSub = 0;    // the mode the voice radios were linked in
    int heardBefore = 0;                 // the receiver that was heard before the sound moved to a voice radio (0: it did not move)
    std::string voiceErr;                // why the last "Use a second radio for voice" did not work
};
Trunk T;

double nowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
std::string tgFile() { return plat::dataDir() + "/trunk_talkgroups.csv"; }

void tgLoad(const std::string& path, bool merge) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) { T.status = "cannot open " + path; return; }
    std::stringstream ss; ss << f.rdbuf();
    std::string err; int skipped = 0;
    const int n = T.ctl.tgs.importCsv(ss.str(), &err, &skipped, merge);
    T.status = std::to_string(n) + " talkgroups read" + (skipped ? ", " + std::to_string(skipped) + " rows skipped. " + err : std::string());
}
void tgSave(const std::string& path) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary);
    if (f) f << T.ctl.tgs.exportCsv();
    else T.status = "cannot write " + path;
}

bool live(const App& a) { return a.rx.standard == 51; }   // the engine reports its standard code minus one
bool liveDmr(const App& a) { return a.rx.standard == 10; }   // DMR: Tier III grants come as CSBKs
bool liveNxdn(const App& a) { return a.rx.standard == 39; }  // NXDN: Type C grants come as VCALL_ASSGN on the control channel
bool liveEdacs(const App& a) { return a.rx.standard == 131; } // EDACS (family 130)
bool liveLtr(const App& a) { return a.rx.standard == 130; }   // LTR (family 129)
bool liveMpt(const App& a) { return a.rx.standard == 128; }   // MPT 1327 (family 127)
bool liveAnalog(const App& a) { return liveEdacs(a) || liveLtr(a) || liveMpt(a); }
bool liveAny(const App& a) { return live(a) || liveDmr(a) || liveNxdn(a) || liveAnalog(a); }

// ---------------------------------------------------------------- voice radios

std::string radioOf(const App& v) { return v.devIdx >= 2 ? radioShortName(v, v.devIdx) : std::string(tr("Test signal")); }

// What the voice-radio rules (dect2/p25/trunk_voice.h) need to know about a receiver tab
dect2::trunk::RxState stateOf(const App& a) {
    dect2::trunk::RxState r;
    r.rxId = a.rxId; r.family = a.family; r.sub = a.sub; r.voiceFor = a.voiceFor;
    r.running = a.engine.running();
    r.radioLost = a.engine.radioLost();
    const bool radio = a.devIdx >= 2 && a.devIdx < (int)a.devices.size();
    r.claimed = !radio || deviceHolder(a.devices[(size_t)a.devIdx]) == a.rxId;
    return r;
}

uint64_t callOnRadioOf(int rxId) {   // the call the follower has on that receiver's radio (0 none)
    for (size_t k = 0; k < T.voice.size(); k++) if (T.voice[k].rxId == rxId) return T.ctl.follower.voiceCall((int)k);
    return 0;
}

// the receiver's volume and mute to its decoder now (the mode's own tick does this for a receiver that is not a voice radio).
// A voice radio plays only while it carries a followed call: never an unfollowed channel.
void pushSound(App& r) {
    if (!r.engine.running()) return;
    const bool m = r.voiceFor ? dect2::trunk::voiceRadioMuted(r.muted, callOnRadioOf(r.rxId)) : r.muted;
    if (r.family == 50) { r.engine.p25().setVolume(r.volume); r.engine.p25().setMuted(m); }
    else if (r.family == 9) { r.engine.dmr().setVolume(r.volume); r.engine.dmr().setMuted(m); }
    else if (r.family == 38) { r.engine.nxdn().setVolume(r.volume); r.engine.nxdn().setMuted(m); }
    else if (r.family == 130) { r.engine.edacs().setVolume(r.volume); r.engine.edacs().setMuted(m); }
    else if (r.family == 129) { r.engine.ltr().setVolume(r.volume); r.engine.ltr().setMuted(m); }
    else if (r.family == 127) { r.engine.mpt1327().setVolume(r.volume); r.engine.mpt1327().setMuted(m); }
}

// An analog system's receiver plays only the call the follower put it on: a voice radio while it carries one, the control receiver while it
// has hopped to one (on the control channel the FM sound would be its data). Phase 2 and the digital modes play what they decode.
void setAnalogVoice(App& r, bool on) {
    if (!r.engine.running()) return;
    if (r.family == 130) r.engine.edacs().setVoice(on);
    else if (r.family == 129) r.engine.ltr().setVoice(on);
    else if (r.family == 127) r.engine.mpt1327().setVoice(on);
}

// Where a Retune sends a receiver: a P25 receiver goes to Phase 2 on the call's slot for a Phase 2 call and back to Phase 1 for anything else
void retuneReceiver(App& r, const dect2::trunk::Action& ac) {
    if (r.family == 50 && r.engine.running()) {
        const dect2::trunk::SiteInfo& si = T.ctl.sys.site();
        r.engine.p25().setPhase2(ac.p25p2, ac.slot < 0 ? 0 : ac.slot, si.wacn < 0 ? 0 : si.wacn, si.sysId < 0 ? 0 : si.sysId, si.nac < 0 ? 0 : si.nac);
    }
    if (dect2::trunk::isAnalogTrunkFamily(r.family)) setAnalogVoice(r, ac.callId != 0);
    tuneFreq(r, ac.freqHz / 1e6);
}

// The sound moves between the control receiver and its voice radios: the receivers that go quiet are muted first, then the one heard
// is turned on, so two of them never play at once.
void hearNow(int id) {
    receiverSetHeard(id);
    std::vector<App*> group;
    if (App* c = receiverById(T.ctlRx)) group.push_back(c);
    for (const auto& vr : T.voice) if (App* v = receiverById(vr.rxId)) group.push_back(v);
    for (App* r : group) if (r->rxId != id) pushSound(*r);
    for (App* r : group) if (r->rxId == id) pushSound(*r);
}

bool heardIsOurs() {   // the user listens to this trunked system (its control receiver or a voice radio), not to another tab
    const App* h = receiverHeard();
    return h && T.ctlRx && (h->rxId == T.ctlRx || h->voiceFor == T.ctlRx);
}

void soundBack() {
    const int back = T.heardBefore;
    T.heardBefore = 0;
    if (back && heardIsOurs() && receiverById(back)) hearNow(back);
}

void recordStop() { dect2::AudioRecorder::instance().stop(); T.recCall = 0; }

// The follower's actions. ctl: the control receiver that runs it now (nullptr: outside its tick: only what ends calls is done, the radios
// are not moved). Every call end comes through here, so a call's recording stops wherever it ended.
void applyActions(App* ctl) {
    using namespace dect2::trunk;
    for (const Action& ac : T.ctl.takeActions()) {
        switch (ac.kind) {
        case Action::Retune:
            if (!ctl) break;
            if (ac.radio < 0) { T.selfWatch = dect2::trunk::VoiceWatch(); retuneReceiver(*ctl, ac); }   // no voice radio: this receiver leaves the control channel for the call
            else if ((size_t)ac.radio < T.voice.size()) {
                if (App* v = receiverById(T.voice[(size_t)ac.radio].rxId)) {
                    RxPrefScope ps(v->rxId);   // its own settings, not this receiver's
                    retuneReceiver(*v, ac);
                }
            }
            break;
        case Action::ReleaseRadio:   // the voice radio waits, silent, where it is
            if ((size_t)ac.radio < T.voice.size()) if (App* v = receiverById(T.voice[(size_t)ac.radio].rxId)) setAnalogVoice(*v, false);
            break;
        case Action::Notice: T.ctl.messages.push_back(ac.text); break;
        case Action::RecordStart: {
            if (!ctl) break;
            dect2::AudioRecorder& rec = dect2::AudioRecorder::instance();
            const dect2::trunk::CallRecord* cr = T.ctl.log.byId(ac.callId);
            const bool p2 = cr && cr->kind == dect2::trunk::SysKind::P25 && cr->slot >= 0;
            if ((live(*ctl) && !(p2 ? dect2::nxdnAmbeAvailable() : dect2::p25ImbeAvailable())) || rec.active()) break;   // no vocoder: there is no sound to save
            dect2::AudioRecOptions o; o.gated = true;
            char nm[96]; time_t tt = time(nullptr); struct tm g;
#ifdef _WIN32
            localtime_s(&g, &tt);
#else
            localtime_r(&tt, &g);
#endif
            char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &g);
            snprintf(nm, sizeof nm, "/recordings/trunk_%s_%s.wav", ac.text.c_str(), ts);
            std::string err;
            if (rec.start(plat::dataDir() + nm, o, err)) { T.recCall = ac.callId; if (auto* r = T.ctl.log.byId(ac.callId)) r->audioPath = plat::dataDir() + nm; }
            break;
        }
        case Action::RecordStop:
            if (T.recCall == ac.callId) recordStop();
            break;
        default: break;   // OpenSlice / CloseSlice / Listen: the wide capture follows audibleCall(); ReleaseRadio: voiceCall(k) says the same
        }
    }
}

// The safety net: a trunk recording whose call is no longer running stops (whatever path ended the call)
void recordWatch() {
    if (!T.recCall) return;
    const dect2::trunk::CallRecord* r = T.ctl.log.byId(T.recCall);
    if (!r || !r->active) recordStop();
}

// Unlinked: Keep (the user's tab now, another mode), Close (stopped at once, so it never plays, feeds or draws the mode screen; the tab
// closes at the start of the next frame and keeps voiceFor until then), Gone (the tab no longer exists)
enum class Unlink { Keep, Close, Gone };

void unlinkVoice(size_t k, const std::string& why, Unlink what, bool notice) {
    if (k >= T.voice.size()) return;
    const int id = T.voice[k].rxId, ctlRx = T.ctlRx;
    const bool wasHeard = receiverHeard() && receiverHeard()->rxId == id;
    T.ctl.dropVoiceRadio((int)k, why.c_str());   // its call ends now; the others keep their radios (moved down by one)
    T.voice.erase(T.voice.begin() + (long)k);
    applyActions(nullptr);                       // its recording stops
    if (App* v = receiverById(id)) {
        if (what == Unlink::Close) { v->engine.stop(); closeReceiver(id); }
        else receiverSetVoiceFor(*v, 0);
    }
    if (wasHeard) {   // the sound goes back to where it was before the call, else to the control receiver
        const int back = T.heardBefore && T.heardBefore != id ? T.heardBefore : ctlRx;
        T.heardBefore = 0;
        if (receiverById(back)) hearNow(back);
    }
    if (T.voice.empty()) T.heardBefore = 0;
    T.ctl.messages.push_back(why);
    if (notice) noticePush(why, {}, 12);
}

void unlinkAll(const std::string& why, Unlink what, bool notice) {
    while (!T.voice.empty()) unlinkVoice(T.voice.size() - 1, why, what, false);
    if (notice && !why.empty()) noticePush(why, {}, 12);
}

// Following is off (or the control receiver stopped): the calls on the voice radios end and the sound goes back; the radios stay linked, silent
void idleVoice(const char* why) {
    bool any = false;
    for (int k = 0; k < T.ctl.follower.voiceRadios(); k++) any |= T.ctl.follower.voiceCall(k) != 0;
    if (!any) return;
    T.ctl.endVoiceCalls(why);
    applyActions(nullptr);
    soundBack();
}

std::string fmtRx(const char* f, int a, int b = 0) { char t[512]; snprintf(t, sizeof t, f, a, b); return t; }

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string dstOf(const dect2::P25Call& c) { return (c.group ? "TG " : "") + dect2::p25Id(c.dst); }
const char* kindOf(const dect2::P25Call& c) { return c.kind == 1 ? "grant" : c.kind == 2 ? "data" : "voice"; }
std::string mhz(double hz) { if (hz <= 0) return "-"; char b[32]; snprintf(b, sizeof b, "%.5f MHz", hz / 1e6); return b; }
std::string cryptoOf(const dect2::P25Call& c) {
    char b[96];
    if (c.algId != dect2::p25::kAlgClear) snprintf(b, sizeof b, "encrypted, %s (0x%02X), key %d", dect2::p25::algName(c.algId), c.algId, c.keyId);
    else snprintf(b, sizeof b, "encrypted");
    return b;
}

void kvRow(const App& a, const char* k, const char* fmt, ...) {
    ImGui::TextDisabled("%s", k); kvColumn(130 * gUi);
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(b); ImGui::PopTextWrapPos(); ImGui::PopFont();
}

void voiceLine() {
    ImGui::AlignTextToFramePadding();
    std::string lib;
    if (!dect2::codec::pluginLoaded("imbe")) dect2::p25ImbeAvailable(&lib);
    appcodec::voiceCodecLine("imbe", "IMBE", lib);
    ImGui::SameLine(0, 4 * gUi);
    codecHelpButton(CodecHelp::P25Imbe);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Eye of SaurCom only receives. It shows what P25 radios send in the clear: talkgroups, unit IDs, the trunking system and,\n"
                          "with your own mbelib, clear voice. Encrypted calls are only reported as encrypted; nothing is decrypted.\n"
                          "Listening to radio services is regulated in many countries: follow the rules where you live.");
}

void nowView(App& a) {
    voiceLine();
    ImGui::Spacing();
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to hear P25"); return; }
    const dect2::P25Telemetry& t = a.rx.p25;
    const dect2::P25Call& n = t.now;
    if (n.wallTime == 0) {
        ImGui::TextDisabled("%s", t.control ? "a control channel: see Calls and Trunking for the voice grants" : "no call heard yet");
        return;
    }
    ImGui::PushFont(a.mono, 0);
    ImGui::TextColored(n.active ? pal::okGreen() : pal::grey(0.6f), "%s  %s  ->  %s", n.active ? "ON AIR" : "last", dect2::p25Id(n.src).c_str(), dstOf(n).c_str());
    ImGui::PopFont();
    kvRow(a, "call", "%s, NAC %03X%s", n.group ? "group" : "unit to unit", n.nac < 0 ? 0 : n.nac, n.lateEntry ? ", joined late" : "");
    if (n.emergency) { ImGui::TextDisabled("note"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "EMERGENCY"); }
    if (n.encrypted) { ImGui::TextDisabled("encryption"); kvColumn(130 * gUi); ImGui::TextColored(pal::warnAmber(), "%s: no sound (not decrypted)", cryptoOf(n).c_str()); }
    kvRow(a, "duration", "%.1f s, %u voice frames", n.durSec, n.voiceFrames);
    kvRow(a, "started", "%s UTC", clock(n.wallTime).c_str());
    kvRow(a, "voice", "%s", n.encrypted ? "-" : n.voice ? "playing (IMBE through mbelib)" : t.imbe ? "-" : "no mbelib: frames counted only");
}

void callsView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see the calls"); return; }
    const auto& log = a.rx.p25.log;
    if (ImGui::Button("Copy")) {
        std::string txt;
        for (const auto& e : log) {
            char b[400];
            snprintf(b, sizeof b, "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%.1f s\n", clock(e.wallTime).c_str(), kindOf(e), dect2::p25Id(e.src).c_str(), dstOf(e).c_str(),
                     e.kind == 1 ? (dect2::p25::channelText(e.channel) + " " + mhz(e.freqHz)).c_str() : "", e.encrypted ? cryptoOf(e).c_str() : "clear", e.text.c_str(), e.durSec);
            txt += b;
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (!ImGui::BeginTable("##p25log", 7 + (wideActive(a) ? 1 : 0), ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollX, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    if (wideActive(a)) ImGui::TableSetupColumn("Carrier"); ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("From"); ImGui::TableSetupColumn("To");
    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Length");
    ImGui::TableHeadersRow();
    auto row = [&](const dect2::P25Call& e, bool on) {
        ImGui::TableNextRow();
        if (wideActive(a)) { ImGui::TableNextColumn(); wideCarrierCell(a, e.carrierId, e.carrierHz); }
        const ImVec4 c = on ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", on ? "on air" : clock(e.wallTime).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(kindOf(e));
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%s", dect2::p25Id(e.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dstOf(e).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (e.kind == 1) ImGui::Text("%s  %s", dect2::p25::channelText(e.channel).c_str(), mhz(e.freqHz).c_str()); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (e.emergency) { ImGui::TextColored(pal::warnAmber(), "EMERGENCY"); ImGui::SameLine(); }
        if (e.encrypted) { ImGui::TextColored(pal::warnAmber(), "%s", cryptoOf(e).c_str()); ImGui::SameLine(); }
        ImGui::TextUnformatted(e.kind == 0 ? (e.voice ? "voice played" : "") : e.text.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", e.durSec);
        if (ImGui::IsItemHovered() && e.kind == 0)
            ImGui::SetTooltip("NAC %03X, %u voice frames, %s%s", e.nac < 0 ? 0 : e.nac, e.voiceFrames, e.endSeen ? "ended with a terminator" : "signal lost", e.lateEntry ? ", joined late" : "");
    };
    if (a.rx.p25.now.active) row(a.rx.p25.now, true);
    for (const auto& e : log) row(e, false);
    ImGui::EndTable();
    if (log.empty() && !a.rx.p25.now.active) ImGui::TextDisabled("nothing heard yet");
}

void trunkingView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::P25Telemetry& t = a.rx.p25;
    const dect2::P25Site& s = t.site;
    if (!s.valid) ImGui::TextDisabled("no system information (a trunked control channel sends it; conventional channels do not)");
    else {
        kvRow(a, "WACN / system", "%05X / %03X", s.wacn < 0 ? 0 : s.wacn, s.sysId < 0 ? 0 : s.sysId);
        kvRow(a, "RFSS / site", "%d / %d", s.rfss, s.site);
        kvRow(a, "NAC", "%03X", s.nac < 0 ? 0 : s.nac);
        kvRow(a, "control channel", "%s  %s", dect2::p25::channelText(s.channel).c_str(), mhz(s.freqHz).c_str());
        kvRow(a, "service class", "0x%02X", s.ssc);
        kvRow(a, "heard", "%s UTC", clock(s.wallTime).c_str());
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Adjacent sites");
    if (t.adjacent.empty()) ImGui::TextDisabled("  none heard");
    else if (ImGui::BeginTable("##p25adj", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("System"); ImGui::TableSetupColumn("RFSS / site"); ImGui::TableSetupColumn("Control channel"); ImGui::TableSetupColumn("Heard");
        ImGui::TableHeadersRow();
        for (const auto& j : t.adjacent) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%03X", j.sysId);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", j.rfss, j.site);
            ImGui::TableNextColumn(); ImGui::Text("%s  %s", dect2::p25::channelText(j.channel).c_str(), mhz(j.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(j.wallTime).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Channel identifiers");
    bool any = false;
    for (const auto& r : t.idens) any = any || r.valid;
    if (!any) ImGui::TextDisabled("  none heard (identifier updates turn channel numbers into frequencies)");
    else if (ImGui::BeginTable("##p25iden", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("ID"); ImGui::TableSetupColumn("Base"); ImGui::TableSetupColumn("Spacing"); ImGui::TableSetupColumn("Transmit offset"); ImGui::TableSetupColumn("Width");
        ImGui::TableHeadersRow();
        for (int i = 0; i < 16; i++) {
            const auto& r = t.idens[i];
            if (!r.valid) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d%s", i, r.vu ? " (VHF/UHF)" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(r.baseHz).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.2f kHz", r.spacingHz / 1e3);
            ImGui::TableNextColumn(); ImGui::Text("%+.4f MHz", r.txOffsetHz / 1e6);
            ImGui::TableNextColumn(); ImGui::Text("%.2f kHz", r.bwHz / 1e3);
        }
        ImGui::EndTable();
    }
}

void talkgroupView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const auto& tg = a.rx.p25.talkgroups;
    if (tg.empty()) { ImGui::TextDisabled("no talkgroups heard yet"); return; }
    if (!ImGui::BeginTable("##p25tg", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Grants"); ImGui::TableSetupColumn("Calls"); ImGui::TableSetupColumn("Last unit");
    ImGui::TableSetupColumn("Last channel"); ImGui::TableSetupColumn("Last heard"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const auto& g : tg) {
        ImGui::TableNextRow();
        const ImVec4 c = g.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(c, "%d", g.tg); ImGui::PopFont();
        ImGui::TableNextColumn();   // the name from the follower's talkgroup list (Follow tab: Import CSV or edit there)
        if (const dect2::trunk::TgEntry* e = T.ctl.tgs.find(g.tg)) ImGui::TextUnformatted(e->alpha.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", g.grants);
        ImGui::TableNextColumn(); ImGui::Text("%u", g.calls);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::p25Id(g.lastSrc).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(g.lastFreqHz).c_str());
        ImGui::TableNextColumn(); ImGui::TextColored(c, "%s", g.active ? "active" : clock(g.lastWall).c_str());
        ImGui::TableNextColumn();
        if (g.emergency) { ImGui::TextColored(pal::warnAmber(), "emergency"); ImGui::SameLine(); }
        if (g.encrypted) ImGui::TextColored(pal::warnAmber(), "encrypted");
    }
    ImGui::EndTable();
}

void messageView(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); return; }
    const auto& ev = a.rx.p25.events;
    ImGui::PushFont(a.mono, 0);
    for (const auto& s : ev) ImGui::TextUnformatted(s.c_str());
    ImGui::PopFont();
    if (ev.empty()) ImGui::TextDisabled("no messages yet");
}

// ---------------------------------------------------------------- hooks

// "number=MHz" pairs (spaces, commas or new lines between them) into a channel table; the number of channels read
template <class Set> int applyChannels(const std::string& text, int maxNumber, Set set) {
    std::string tok, norm = text;
    int n = 0;
    for (char& c : norm) if (c == ',' || c == ';' || c == '\n' || c == '\t') c = ' ';
    std::istringstream words(norm);
    while (words >> tok) {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) continue;
        char* end = nullptr;
        const long lcn = std::strtol(tok.c_str(), &end, 10);
        const double mhz = std::strtod(tok.c_str() + eq + 1, nullptr);
        if (end == tok.c_str() || end != tok.c_str() + eq || lcn < 0 || lcn > maxNumber || !(mhz > 1 && mhz < 6000)) continue;
        set((int)lcn, mhz * 1e6);
        n++;
    }
    return n;
}
// The channel lists ("LCN,frequency" lines; trunk::ChannelTable reads them) into the models; the number of channels taken, why lines were skipped
int applyDmrChannels(const std::string& text, std::string* err = nullptr) {
    dect2::trunk::ChannelTable t;
    const int n = t.parse(text, 4095, err);
    T.ctl.dmr = dect2::trunk::DmrModel();
    for (const auto& kv : t.entries()) T.ctl.dmr.setChannel(kv.first, kv.second);
    return n;
}
int applyNxdnChannels(const std::string& text, std::string* err = nullptr) {
    dect2::trunk::ChannelTable t;
    const int n = t.parse(text, 1023, err);
    T.ctl.nxdn = dect2::trunk::NxdnModel();
    for (const auto& kv : t.entries()) T.ctl.nxdn.setChannel(kv.first, kv.second);
    return n;
}
int applyEdacsChannels(const std::string& text, std::string* err = nullptr) { return T.ctl.edacs.lcns.parse(text, 31, err); }
int applyLtrChannels(const std::string& text, std::string* err = nullptr) { return T.ctl.ltr.repeaters.parse(text, 20, err); }
int applyMptChannels(const std::string& text, std::string* err = nullptr) {
    const int n = T.ctl.mpt.channels.parse(text, 1023, err);
    T.ctl.mpt.channels.setFormula(T.mptBaseMhz * 1e6, T.mptSpacingKhz * 1e3);
    return n;
}
void saveTables() {   // the lists belong to the system, not to a receiver: kept with RX 1's settings
    RxPrefScope ps(1);
    plat::prefs().setS("trunkDmrChannels", T.dmrChannels);
    plat::prefs().setS("trunkNxdnChannels", T.nxdnChannels);
    plat::prefs().setS("trunkEdacsLcns", T.edacsChannels);
    plat::prefs().setS("trunkLtrRepeaters", T.ltrChannels);
    plat::prefs().setS("trunkMptChannels", T.mptChannels);
    plat::prefs().setD("trunkMptBase", T.mptBaseAuto ? 0.0 : T.mptBaseMhz);
    plat::prefs().setD("trunkMptSpacing", T.mptSpacingKhz);
    plat::prefs().setI("trunkEdacsNarrow", T.edacsNarrow ? 1 : 0);
}

// The analog modes' receivers: the narrowband setting and whether they may play (setAnalogVoice). Runs every frame, following or not.
void analogSound(App& a) {
    if (!dect2::trunk::isAnalogTrunkFamily(a.family) || !a.engine.running()) return;
    if (a.family == 130) a.engine.edacs().setNarrow(T.edacsNarrow);
    uint64_t id = 0;
    if (a.voiceFor) id = callOnRadioOf(a.rxId);
    else if (T.on && T.ctlRx == a.rxId && T.ctl.follower.state() == dect2::trunk::FollowState::Hopped) id = T.ctl.follower.audibleCall();
    const dect2::trunk::CallRecord* r = id ? T.ctl.log.byId(id) : nullptr;
    setAnalogVoice(a, r && r->active && !r->provoice);
    pushSound(a);
}

// What a receiver of an analog system hears of the call it was sent to: its channel, its carrier, the channel's end signalling (EDACS: data
// frames, the drop burst; LTR: the turn-off of this talkgroup; MPT 1327: CLEAR after SYNT)
dect2::trunk::VoiceHeard analogHeard(const App& v, dect2::trunk::VoiceWatch& w, const dect2::trunk::CallRecord& r) {
    using namespace dect2::trunk;
    auto on = [&](double hz) { return hz > 0 && std::fabs(hz - r.freqHz) < 2500; };
    if (liveEdacs(v)) { const auto& t = v.rx.edacs; return analogVoiceHeard(w, r.id, on(t.channelHz), t.voice.carrier, t.blocksOk, true, v.rx.seq); }
    if (liveLtr(v)) {
        const auto& t = v.rx.ltr;
        const bool ours = t.turnOffId >= 0 && dect2::ltr::tgNumber(t.turnOffArea, t.turnOffHome, t.turnOffId) == r.tg;
        return analogVoiceHeard(w, r.id, on(t.channelHz), t.voice.carrier, t.turnOffs, ours, v.rx.seq);
    }
    if (liveMpt(v)) { const auto& t = v.rx.mpt1327; return analogVoiceHeard(w, r.id, on(t.channelHz), t.voice.carrier, t.clears, true, v.rx.seq); }
    return VoiceHeard::Nothing;
}

// What a voice radio found out about its call that the control channel did not say: P25 encryption (MAC_PTT, the encryption sync), EDACS ProVoice
void noteVoiceRadio(App& v, uint64_t id) {
    dect2::trunk::CallRecord* r = T.ctl.log.byId(id);
    if (!r) return;
    if (live(v) && v.rx.p25.now.active && v.rx.p25.now.encrypted && (v.rx.p25.now.dst < 0 || v.rx.p25.now.dst == r->tg) && !r->encrypted) {
        r->encrypted = true;
        r->note = "encrypted (no sound)";
    }
    if (liveEdacs(v) && v.rx.edacs.provoice && !r->provoice && std::fabs(v.rx.edacs.channelHz - r->freqHz) < 2500) {
        r->provoice = true;
        r->note = "ProVoice (EDACS digital voice): there is no ProVoice decoder";
        setAnalogVoice(v, false);
    }
}

void trunkTick(App& a) {
    if (!T.loaded) {
        T.loaded = true;
        if (std::filesystem::exists(std::filesystem::u8path(tgFile()))) tgLoad(tgFile(), false);
        RxPrefScope ps(1);
        T.dmrChannels = plat::prefs().getS("trunkDmrChannels", "");
        applyDmrChannels(T.dmrChannels);
        T.nxdnChannels = plat::prefs().getS("trunkNxdnChannels", "");
        applyNxdnChannels(T.nxdnChannels);
        T.edacsChannels = plat::prefs().getS("trunkEdacsLcns", "");
        applyEdacsChannels(T.edacsChannels);
        T.ltrChannels = plat::prefs().getS("trunkLtrRepeaters", "");
        applyLtrChannels(T.ltrChannels);
        T.mptBaseMhz = plat::prefs().getD("trunkMptBase", 0.0);
        T.mptSpacingKhz = plat::prefs().getD("trunkMptSpacing", 12.5);
        T.mptChannels = plat::prefs().getS("trunkMptChannels", "");
        applyMptChannels(T.mptChannels);
        T.edacsNarrow = plat::prefs().getI("trunkEdacsNarrow", 0) != 0;
    }
    analogSound(a);
    // a control receiver left on a Phase 2 voice channel (following switched off while it was there): back to Phase 1, so a control channel decodes
    if (live(a) && !a.voiceFor && a.rx.p25.phase2 && !(T.on && T.ctlRx == a.rxId && T.ctl.follower.state() == dect2::trunk::FollowState::Hopped))
        a.engine.p25().setPhase2(false);
    const bool p25 = live(a), dmr = liveDmr(a), nxdn = liveNxdn(a), edacs = liveEdacs(a), ltr = liveLtr(a), mpt = liveMpt(a);
    if (mpt && T.mptBaseMhz <= 0 && !a.voiceFor && a.rx.mpt1327.locked && T.ctl.follower.state() != dect2::trunk::FollowState::Hopped) {
        // MPT 1327 with no base typed: when the control channel broadcasts one control channel number only, it is this one (channel n is here)
        int n = 0, ch = -1;
        for (const dect2::MptChannelRow& c : a.rx.mpt1327.channels) if (c.controlChannel) { n++; ch = c.channel; }
        if (n == 1 && ch > 0 && a.rx.mpt1327.channelHz > 0) {
            T.mptBaseMhz = (a.rx.mpt1327.channelHz - ch * T.mptSpacingKhz * 1e3) / 1e6;
            T.mptBaseAuto = true;
            applyMptChannels(T.mptChannels);
        }
    }
    if (!T.on || !liveAny(a)) return;
    using namespace dect2::trunk;
    // One receiver drives the follower: a running one in a trunked mode, not a voice radio, and while the one that drives it can, no other
    // (a stopped tab's telemetry is frozen; a voice radio's control messages are never taken)
    {
        const App* o = T.ctlRx ? receiverById(T.ctlRx) : nullptr;
        RxState os;
        if (o) os = stateOf(*o);
        if (!controlMayFeed(stateOf(a), o ? &os : nullptr)) return;
        if (T.ctlRx != a.rxId) {
            if (!T.voice.empty()) unlinkAll(fmtRx(tr("RX %d no longer follows the system: its voice radios are unlinked and closed."), T.ctlRx), Unlink::Close, true);
            if (skipBacklogOnTakeover(T.ctlRx, a.rxId)) {   // its stored messages (old grants) are not new: start from its latest, as the scanner does
                Trunk::Seen& s0 = T.seen[a.rxId];
                s0.tsbk = a.rx.p25.tsbks.empty() ? 0 : a.rx.p25.tsbks.back().seq;
                s0.csbk = a.rx.dmr.csbks.empty() ? 0 : a.rx.dmr.csbks.back().seq;
                s0.nxdn = a.rx.nxdn.grants.empty() ? 0 : a.rx.nxdn.grants.back().seq;
                s0.nxdnCac = a.rx.nxdn.cacOk;
                s0.edacs = a.rx.edacs.msgs.empty() ? 0 : a.rx.edacs.msgs.back().seq;
                s0.ltr = a.rx.ltr.words.empty() ? 0 : a.rx.ltr.words.back().seq;
                s0.mpt = a.rx.mpt1327.msgs.empty() ? 0 : a.rx.mpt1327.msgs.back().seq;
            }
            T.ctlRx = a.rxId;
        }
    }
    if (T.ctl.follower.voiceRadios() != (int)T.voice.size()) T.ctl.setVoiceRadios((int)T.voice.size());
    const double now = nowSec();
    const dect2::P25Telemetry& t = a.rx.p25;
    Trunk::Seen& sn = T.seen[a.rxId];   // this receiver's own messages (the sequence numbers are shared by every receiver)
    // a restarted receiver counts its messages from 1 again
    if (!t.tsbks.empty() && t.tsbks.back().seq < sn.tsbk) sn.tsbk = 0;
    if (!a.rx.dmr.csbks.empty() && a.rx.dmr.csbks.back().seq < sn.csbk) sn.csbk = 0;
    if (!a.rx.nxdn.grants.empty() && a.rx.nxdn.grants.back().seq < sn.nxdn) sn.nxdn = 0;
    // the analog systems: what the control receiver decoded (its own sequence numbers are unique across receivers)
    if (edacs) for (const dect2::EdacsMsgRec& m : a.rx.edacs.msgs) { if (m.seq <= sn.edacs) continue; sn.edacs = m.seq; T.ctl.onEdacs(m.data, now); }
    if (ltr) for (const dect2::LtrWordRec& w : a.rx.ltr.words) { if (w.seq <= sn.ltr) continue; sn.ltr = w.seq; T.ctl.onLtr(w.area, w.gotoCh, w.home, w.id, w.freeCh, now); }
    if (mpt) for (const dect2::MptMsgRec& m : a.rx.mpt1327.msgs) {
        if (m.seq <= sn.mpt) continue;
        sn.mpt = m.seq;
        T.ctl.onMpt(m.type, m.calledPfix, m.calledIdent, m.callerIdent, m.channel, m.dataCall, m.traffic, now);
    }
    if (p25) for (const auto& r : t.tsbks) {
        if (r.seq <= sn.tsbk) continue;
        sn.tsbk = r.seq;
        dect2::p25::Tsbk k; k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
        T.ctl.onTsbk(k, r.nac, now);
    }
    if (dmr) for (const auto& r : a.rx.dmr.csbks) {
        if (r.seq <= sn.csbk) continue;
        sn.csbk = r.seq;
        T.ctl.onDmrCsbk(r.opcode, r.fid, r.data, now);
    }
    if (nxdn) {
        // a Type C control channel is on air when new control channel (CAC) frames decode; site info alone is kept from earlier
        if (a.rx.nxdn.cacOk < sn.nxdnCac) sn.nxdnCac = 0;
        if (a.rx.nxdn.cacOk > sn.nxdnCac) { sn.nxdnCac = a.rx.nxdn.cacOk; T.ctl.onControlSeen(now); }
        for (const auto& g : a.rx.nxdn.grants) {
            if (g.seq <= sn.nxdn) continue;
            sn.nxdn = g.seq;
            T.ctl.onNxdnAssignment(g.callType, g.src, g.dst, g.channel, g.ccOption, now);
        }
    }
    T.ctl.setCapture(radioCenterMhz(a) * 1e6, p25 && wideActive(a) ? t.inputRate : 0);   // DMR, NXDN: the radio retunes for each call
    const uint64_t audible = T.ctl.follower.audibleCall();
    if (const uint64_t id = T.ctl.follower.voiceRadioOf(audible) < 0 ? audible : 0; id && (edacs || ltr || mpt)) {   // hopped to an analog call
        if (const CallRecord* r = T.ctl.log.byId(id)) {
            const VoiceHeard h = analogHeard(a, T.selfWatch, *r);
            if (h == VoiceHeard::Voice) T.ctl.onVoice(id, now);
            else if (h == VoiceHeard::Ended) T.ctl.onTerminator(id, now);
        }
    } else if (const uint64_t id = T.ctl.follower.voiceRadioOf(audible) < 0 ? audible : 0) {   // on the call's channel: voice heard keeps the call (and the radio) there
        const CallRecord* r = T.ctl.log.byId(id);
        bool voice = false;
        // only this call's own voice counts: the channel may already carry another talkgroup's call (its grant was not heard here)
        auto ours = [&](long long dst) { return r && (dst < 0 || dst == r->tg); };
        if (p25) voice = t.now.active && t.now.kind == 0 && ours(t.now.dst);
        else if (dmr) voice = r && r->slot >= 0 && r->slot < 2 && a.rx.dmr.slot[r->slot].active && a.rx.dmr.slot[r->slot].state == 1 && ours(a.rx.dmr.slot[r->slot].dst ? (long long)a.rx.dmr.slot[r->slot].dst : -1);   // 0: not decoded yet
        else voice = a.rx.nxdn.now.active && a.rx.nxdn.now.kind == 0 && ours(a.rx.nxdn.now.dst);
        if (voice && !(p25 && wideActive(a))) T.ctl.onVoice(id, now);
    }
    // the voice radios: each one's own receiver says whether its call is still talking, or sent its terminator. Only a linked, running
    // receiver holding its radio counts, and only its new reports (rx.seq): frozen telemetry keeps nothing alive and ends nothing.
    const RxState cs = stateOf(a);
    for (size_t k = 0; k < T.voice.size(); k++) {
        Trunk::VoiceRadio& vr = T.voice[k];
        App* v = receiverById(vr.rxId);
        const uint64_t id = T.ctl.follower.voiceCall((int)k);
        const CallRecord* r = id ? T.ctl.log.byId(id) : nullptr;
        if (!v || !r || !voiceMayFeed(stateOf(*v), cs)) continue;
        VoiceHeard h = VoiceHeard::Nothing;
        if (p25 && live(*v)) h = p25VoiceHeard(vr.watch, id, r->tg, v->rx.p25.now, v->rx.seq);
        else if (dmr && liveDmr(*v)) h = dmrVoiceHeard(vr.watch, id, r->tg, r->slot, v->rx.dmr.slot, v->rx.seq);
        else if (nxdn && liveNxdn(*v)) h = nxdnVoiceHeard(vr.watch, id, r->tg, v->rx.nxdn.now, v->rx.seq);
        else if ((edacs && liveEdacs(*v)) || (ltr && liveLtr(*v)) || (mpt && liveMpt(*v))) h = analogHeard(*v, vr.watch, *r);
        noteVoiceRadio(*v, id);
        if (h == VoiceHeard::Voice) T.ctl.onVoice(id, now);
        else if (h == VoiceHeard::Ended) T.ctl.onTerminator(id, now);
    }
    T.ctl.tick(now);
    applyActions(&a);
    if (!T.voice.empty()) {   // the sound follows the call: the voice radio that carries it is heard, then the sound goes back
        const int k = T.ctl.follower.voiceRadioOf(T.ctl.follower.audibleCall());
        App* want = k >= 0 && (size_t)k < T.voice.size() ? receiverById(T.voice[(size_t)k].rxId) : nullptr;
        App* h = receiverHeard();
        if (want && h != want && heardIsOurs()) {
            if (!T.heardBefore) T.heardBefore = h->rxId;
            hearNow(want->rxId);
        } else if (!want && T.heardBefore) soundBack();
    }
    if (wideActive(a)) {   // the sound follows the call that should be heard
        const uint64_t id = T.ctl.follower.audibleCall();
        const CallRecord* r = id ? T.ctl.log.byId(id) : nullptr;
        if (r) {
            const double ctr = radioCenterMhz(a) * 1e6;
            const dect2::WideCarrier* best = nullptr;
            for (const auto& c : a.rx.wide) if (std::fabs(ctr + c.hz - r->freqHz) < 7000 && (!best || c.levelDb > best->levelDb)) best = &c;
            if (best) { a.wideSel = best->id; T.forcedSel = best->id; }
        } else if (T.forcedSel) { a.wideSel = 0; T.forcedSel = 0; }
    }
    if (T.dirty) { T.dirty = false; tgSave(tgFile()); }
}

// The Follow tab's voice radios: the one-click button, and a line per linked radio with Unlink (docs/trunking.md, "Several radios")
void voiceRadiosView(App& a) {
    using namespace dect2::trunk;
    if (!T.voice.empty() && T.ctlRx != a.rxId) {   // another receiver holds the control channel for these radios
        ImGui::TextDisabled("%s", fmtRx(tr("RX %d is on the control channel and follows the calls with its voice radios."), T.ctlRx).c_str());
        return;
    }
    for (size_t k = 0; k < T.voice.size(); k++) {
        const App* v = receiverById(T.voice[k].rxId);
        if (!v) continue;
        ImGui::PushID((int)k);
        ImGui::AlignTextToFramePadding();
        char line[300];
        snprintf(line, sizeof line, tr("Voice: RX %d \xC2\xB7 %s"), v->rxId, radioOf(*v).c_str());   // the radio's name is the user's text: an argument, never the format
        ImGui::TextColored(pal::okGreen(), "%s", line);
        if (const uint64_t id = T.ctl.follower.voiceCall((int)k)) if (const CallRecord* r = T.ctl.log.byId(id)) {
            ImGui::SameLine(0, 8 * gUi);
            const TgEntry* e = r->group ? T.ctl.tgs.find(r->tg) : nullptr;
            ImGui::TextDisabled("TG %d %s  %s", r->tg, e ? e->alpha.c_str() : "", mhz(r->freqHz).c_str());
        }
        ImGui::SameLine(0, 10 * gUi);
        if (ImGui::SmallButton(tr("Unlink"))) {
            unlinkVoice(k, fmtRx(tr("RX %d is no longer a voice radio: its tab is closed."), v->rxId), Unlink::Close, false);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }
    const App* o = T.ctlRx ? receiverById(T.ctlRx) : nullptr;
    RxState os;
    if (o) os = stateOf(*o);
    const bool on = liveAny(a) && controlMayFeed(stateOf(a), o ? &os : nullptr);   // running, and driving the follower
    ImGui::BeginDisabled(!on);
    if (ImGui::Button(T.voice.empty() ? tr("Use a second radio for voice") : tr("Use another radio for voice"))) {
        std::string why;
        if (App* v = addVoiceReceiver(a, &why)) {
            if (T.voice.empty()) { T.linkFamily = a.family; T.linkSub = a.sub; }
            T.ctlRx = a.rxId;
            Trunk::VoiceRadio vr;
            vr.rxId = v->rxId;
            vr.linkedSec = nowSec();
            T.voice.push_back(vr);
            T.ctl.setVoiceRadios((int)T.voice.size());
            T.on = true;   // the point of the click: follow the calls
            T.voiceErr.clear();
            T.ctl.messages.push_back(fmtRx(tr("RX %d follows the calls as a voice radio; RX %d stays on the control channel."), v->rxId, a.rxId));
        } else T.voiceErr = why;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("%s", on ? tr("Opens a receiver tab on the first free radio, in this mode, and links it: this radio stays on the control channel and every call is heard on the other one. Press again to add one more.")
                                   : tr("Start this receiver on the control channel first."));
    if (!T.voiceErr.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", T.voiceErr.c_str());
        ImGui::PopTextWrapPos();
    }
    if (T.voice.empty() && !wideActive(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("With one radio this receiver leaves the control channel for each call and misses the calls granted meanwhile. "
                                     "A second radio for voice avoids that; so does a wide-band radio (HackRF) with whole-span decoding."));
        ImGui::PopTextWrapPos();
    } else if (!T.voice.empty()) {
        ImGui::TextDisabled("%s", tr("This radio stays on the control channel. When every voice radio is busy, a higher priority call takes the one with the lowest priority call; the others are logged as missed."));
    }
}

// a call's talkgroup as the system writes it: "TG 1201", "unit 4321", EDACS "05-045", LTR "0-03-017", MPT 1327 "5/1234"
std::string tgText(const dect2::trunk::CallRecord& r) {
    if (!r.label.empty()) return std::string(dect2::trunk::sysKindName(r.kind)) + " " + r.label;
    return (r.group ? "" : "unit ") + std::to_string(r.tg);
}

int textResize(ImGuiInputTextCallbackData* d) {   // ImGui's InputText over a std::string
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(d->UserData);
        str->resize((size_t)d->BufTextLen);
        d->Buf = str->data();
    }
    return 0;
}

// The system's channel list: one "LCN,frequency" line per channel, typed or pasted (or read from a file), applied and saved with one button.
// known: how many channels the follower knows now (0: says plainly what is needed). apply: parses the text into the model.
void channelListUi(const char* id, std::string& text, int known, int (*apply)(const std::string&, std::string*), const char* what, bool formula = false) {
    ImGui::PushID(id);
    if (known == 0 && formula) {
        ImGui::TextDisabled("%s", tr("The formula gives every channel; list here only the channels that do not follow it."));
    } else if (known == 0) {
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Add your system's channel list"));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("The control channel names channels by number. One line per channel: the number, a comma, the frequency in MHz (for example 1,851.0125). Paste the list from your system's listing, or load a CSV file."));
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextDisabled(tr("%d channels in the list (number,MHz per line)"), known);
    }
    ImGui::InputTextMultiline("##list", text.data(), text.capacity() + 1, ImVec2(-1, 5.5f * ImGui::GetTextLineHeightWithSpacing()), ImGuiInputTextFlags_CallbackResize, textResize, &text);
    if (ImGui::Button(tr("Apply and save"))) {
        std::string err;
        const int n = apply(text, &err);
        saveTables();
        T.status = std::to_string(n) + " " + what + (err.empty() ? std::string() : ". " + err);
        for (char& c : T.status) if (c == '\n') c = ' ';
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Load file"))) {
        const std::string path = openFileDialog();
        if (!path.empty()) {
            std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            if (f) {
                text = ss.str();
                std::string err;
                const int n = apply(text, &err);
                saveTables();
                T.status = std::to_string(n) + " " + what + " read from " + path;
            } else T.status = "cannot open " + path;
        }
    }
    ImGui::PopID();
}

void followView(App& a) {
    using namespace dect2::trunk;
    ImGui::Checkbox("Follow calls", &T.on);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follows the calls a trunked system's control channel announces: P25 (Phase 1 and Phase 2), DMR Tier III, NXDN Type C, EDACS, LTR and MPT 1327.\n"
                          "Inside a wide capture every channel is decoded and the sound moves to the call you should hear; with a second radio the calls go to it;\n"
                          "with one radio it leaves the control channel for the call and comes back. Encrypted calls are logged as encrypted and never followed.\n"
                          "Digital voice needs your own vocoder (docs/CODECS.md); analog systems (EDACS, LTR, MPT 1327) play their FM voice. See docs/trunking.md.");
    if (!T.on) { ImGui::TextDisabled("%s", liveAny(a) ? "off: the control channel is only decoded, nothing is followed" : "start the receiver first"); }
    if (liveDmr(a)) {
        ImGui::TextDisabled("DMR Tier III: the grants give logical channel numbers (LCN).");
        channelListUi("dmr", T.dmrChannels, (int)[] { int n = 0; for (int i = 0; i < 4096; i++) n += T.ctl.dmr.lcnFreq(i) > 0; return n; }(), applyDmrChannels, "DMR channels");
        ImGui::TextDisabled("%llu Tier III grants seen", (unsigned long long)T.ctl.dmr.grants());
    }
    if (liveNxdn(a)) {
        ImGui::TextDisabled("NXDN Type C: the channel assignments give channel numbers.");
        channelListUi("nxdn", T.nxdnChannels, (int)[] { int n = 0; for (int i = 0; i < 1024; i++) n += T.ctl.nxdn.channelFreq(i) > 0; return n; }(), applyNxdnChannels, "NXDN channels");
        ImGui::TextDisabled("%llu NXDN channel assignments seen", (unsigned long long)T.ctl.nxdn.grants());
    }
    if (liveEdacs(a)) {
        ImGui::TextDisabled("%s", tr("EDACS: calls are assigned to logical channel numbers (LCN 1 to 31)."));
        channelListUi("edacs", T.edacsChannels, (int)T.ctl.edacs.lcns.size(), applyEdacsChannels, "EDACS channels");
        if (ImGui::Checkbox(tr("Narrowband (EDACS NB, 4800 bit/s)"), &T.edacsNarrow)) saveTables();
        if (T.ctl.edacs.sysId() >= 0) kvRow(a, "system", "%04X, site %d", T.ctl.edacs.sysId(), T.ctl.edacs.site());
        const dect2::EdacsTelemetry& et = a.rx.edacs;
        ImGui::TextDisabled("%llu calls assigned; messages: %llu checked by their BCH, %llu on agreeing copies only, %llu refused",
                            (unsigned long long)T.ctl.edacs.grants(), (unsigned long long)et.bchOk, (unsigned long long)et.copiesOnly, (unsigned long long)et.msgBad);
    }
    if (liveLtr(a)) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", tr("LTR has no control channel: start this receiver on a busy repeater of the system (the home repeater of the talkgroups you follow). The data under its voice says which repeater each call is on."));
        ImGui::PopTextWrapPos();
        channelListUi("ltr", T.ltrChannels, (int)T.ctl.ltr.repeaters.size(), applyLtrChannels, "LTR repeaters");
        ImGui::TextDisabled("%llu calls heard (talkgroups are area-home-id, kept in the list as area x 100000 + home x 1000 + id)", (unsigned long long)T.ctl.ltr.grants());
    }
    if (liveMpt(a)) {
        ImGui::TextDisabled("%s", tr("MPT 1327: channel n is on base + n x spacing; list the channels that are not."));
        float base = (float)T.mptBaseMhz, sp = (float)T.mptSpacingKhz;
        ImGui::SetNextItemWidth(140 * gUi);
        bool ch = ImGui::InputFloat(tr("base MHz"), &base, 0, 0, "%.5f");
        ImGui::SameLine(0, 12 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        ch |= ImGui::InputFloat(tr("spacing kHz"), &sp, 0, 0, "%.2f");
        if (ch) { T.mptBaseMhz = base; T.mptSpacingKhz = sp > 0 ? sp : 12.5; T.mptBaseAuto = false; applyMptChannels(T.mptChannels); saveTables(); }
        if (T.mptBaseAuto) ImGui::TextDisabled("%s", tr("The base was worked out from the control channel's broadcast of its own number: check it."));
        else if (T.mptBaseMhz <= 0) ImGui::TextColored(pal::warnAmber(), "%s", tr("Enter the base frequency (channel 0) of your system, or list its channels below."));
        channelListUi("mpt", T.mptChannels, (int)T.ctl.mpt.channels.size(), applyMptChannels, "MPT 1327 channels", T.mptBaseMhz > 0);
        ImGui::TextDisabled("%llu GTC (go to channel) seen", (unsigned long long)T.ctl.mpt.grants());
    }
    const double now = nowSec();
    const ControllerStatus st = T.ctl.status(now);
    kvRow(a, "state", "%s%s", followStateName(st.state), st.controlLocked ? "" : " (no control channel message lately)");
    kvRow(a, "control channel", "%llu good, %llu bad (%.0f %% ok), %u calls on air, %llu not followed",
          (unsigned long long)st.tsbkGood, (unsigned long long)st.tsbkBad, st.crcOkPct, (unsigned)st.activeCalls, (unsigned long long)st.missed);
    const SiteInfo& si = T.ctl.sys.site();
    if (si.valid) kvRow(a, "site", "WACN %05X system %03X RFSS %d site %d NAC %03X", si.wacn < 0 ? 0 : si.wacn, si.sysId < 0 ? 0 : si.sysId, si.rfss, si.site, si.nac < 0 ? 0 : si.nac);
    if (wideActive(a)) ImGui::TextDisabled("wide capture: calls inside the span are followed by the carrier decoders (up to %d at once)", T.ctl.follower.config().maxSlices);
    else if (T.voice.empty()) ImGui::TextDisabled("single channel: the radio leaves the control channel for each call (turn on whole-span decoding to follow inside the capture)");
    voiceRadiosView(a);
    FollowConfig fc = T.ctl.follower.config();
    ImGui::SetNextItemWidth(110 * gUi); bool ch = ImGui::SliderInt("simultaneous calls", &fc.maxSlices, 1, 4);
    ImGui::SameLine(0, 10 * gUi); ImGui::SetNextItemWidth(110 * gUi); ch |= ImGui::SliderFloat("hang time s", &S.hang, 0.5f, 6.f, "%.1f");
    fc.hangSec = S.hang;
    ch |= ImGui::Checkbox("retune the radio", &fc.allowRetune);
    ImGui::SameLine(0, 10 * gUi);
    bool so = T.ctl.tgs.scanOnly();
    if (ImGui::Checkbox("scan list only", &so)) T.ctl.tgs.setScanOnly(so);
    if (ch) T.ctl.follower.setConfig(fc);
    ImGui::Spacing();
    ImGui::TextDisabled("Now talking");
    const auto nt = T.ctl.nowTalking();
    if (nt.empty()) ImGui::TextDisabled("  nobody");
    else if (ImGui::BeginTable("##tnow", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Unit"); ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const CallRecord* r : nt) {
            const TgEntry* e = T.ctl.tgs.find(r->tg);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tgText(*r).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e ? e->alpha.c_str() : "");
            ImGui::TableNextColumn(); ImGui::Text("%d", r->src);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(r->freqHz).c_str());
            ImGui::TableNextColumn();
            if (r->encrypted) ImGui::TextColored(pal::warnAmber(), "encrypted: not followed");
            else if (r->provoice) ImGui::TextColored(pal::warnAmber(), "ProVoice: no decoder");
            else if (r->followed) ImGui::TextColored(pal::okGreen(), "following");
            else ImGui::TextDisabled("%s", r->note.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Talkgroup list");
    if (ImGui::Button("Import CSV")) { const std::string p = openFileDialog(); if (!p.empty()) { tgLoad(p, true); T.dirty = true; } }
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) { const std::string p = saveFileDialog("talkgroups.csv"); if (!p.empty()) tgSave(p); }
    ImGui::SameLine();
    if (ImGui::Button("Add heard")) {   // the talkgroups heard, with the system's own way of writing them as the name
        for (const auto& g : a.rx.p25.talkgroups) T.ctl.tgs.upsert(g.tg);
        for (const CallRecord& r : T.ctl.log.records()) {
            if (!r.group || r.tg <= 0) continue;
            const bool fresh = !T.ctl.tgs.find(r.tg);
            TgEntry& e = T.ctl.tgs.upsert(r.tg);
            if (fresh && !r.label.empty()) e.alpha = r.label;
        }
        T.dirty = true;
    }
    if (!T.status.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", T.status.c_str()); }
    if (ImGui::BeginTable("##ttg", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, 170 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Listen"); ImGui::TableSetupColumn("Lockout");
        ImGui::TableSetupColumn("Priority"); ImGui::TableSetupColumn("Tag");
        ImGui::TableHeadersRow();
        std::vector<TgEntry>& v = const_cast<std::vector<TgEntry>&>(T.ctl.tgs.entries());
        for (TgEntry& e : v) {
            ImGui::PushID(e.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d", e.id);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.alpha.c_str());
            ImGui::TableNextColumn(); if (ImGui::Checkbox("##l", &e.listen)) T.dirty = true;
            ImGui::TableNextColumn(); if (ImGui::Checkbox("##x", &e.lockout)) T.dirty = true;
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(70 * gUi); if (ImGui::DragInt("##p", &e.priority, 0.1f, 0, 10, e.priority ? "%d" : "none")) T.dirty = true;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.tag.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Call log");
    if (ImGui::Button("Export log CSV")) { const std::string p = saveFileDialog("call_log.csv"); if (!p.empty()) { std::ofstream f(std::filesystem::u8path(p), std::ios::binary); f << T.ctl.log.exportCsv(&T.ctl.tgs); } }
    if (ImGui::BeginTable("##tlog", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, std::max(120.f * gUi, ImGui::GetContentRegionAvail().y)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Talkgroup"); ImGui::TableSetupColumn("Unit"); ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Channel");
        ImGui::TableSetupColumn("Encrypted"); ImGui::TableSetupColumn("Notes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        const auto& rs = T.ctl.log.records();
        for (size_t i = rs.size(); i-- > 0;) {
            const CallRecord& r = rs[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(r.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(tgText(r).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", r.src);
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", r.active ? std::max(r.durSec, r.lastSec - r.startSec) : r.durSec);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(r.freqHz).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.encrypted ? "yes" : "");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.note.c_str());
        }
        ImGui::EndTable();
    }
}

// Publish the names of this system's talkgroups to the context store, so that when the sound of a trunked call reaches the speech engine
// (through the audio tap, as "general") the engine has already been told what the dispatcher calls things. The talkgroup list lives here, so
// the feeder lives here too rather than handing the list out to another module.
void contextTick(App& a) {
    static double last = 0;
    const double now = ImGui::GetTime();
    if (now - last < 2.0) return;
    last = now;
    const int64_t nowMs = (int64_t)time(nullptr) * 1000;
    std::vector<dect2::ContextTerm> terms;
    const dect2::P25Telemetry& t = a.rx.p25;
    // The talkgroups that have actually been active, named from the user's list where it has a name for them.
    for (const dect2::P25Talkgroup& g : t.talkgroups) {
        const dect2::trunk::TgEntry* e = T.ctl.tgs.find(g.tg);
        if (!e || e->alpha.empty()) continue;
        const int64_t seen = g.lastWall > 0 ? g.lastWall * 1000 : nowMs;
        terms.push_back({e->alpha, g.active ? 2.0f : 1.0f, seen});
        // The description is often the plain-language name a dispatcher says ("Engine 7", "North Patrol").
        if (!e->desc.empty() && e->desc != e->alpha) terms.push_back({e->desc, 0.7f, seen});
    }
    if (terms.empty()) dect2::ContextStore::global().clearScope("general");
    else dect2::ContextStore::global().setTerms("general", std::move(terms));
}

void tick(App& a) {
    if (live(a)) appcodec::voiceCodecTick(a.rx.p25.phase2 ? "ambe2" : "imbe");   // the user's codec plug-in (docs/CODECS.md), loaded whichever tab is shown
    contextTick(a);
    trunkTick(a);
    if (!a.engine.running()) { S.wasRunning = false; return; }
    dect2::P25Receiver& r = a.engine.p25();
    if (!S.wasRunning || a.volume != S.pushedVol || a.muted != S.pushedMute) {   // the sound controls are shared with the other modes
        r.setVolume(a.volume); r.setMuted(a.muted);
        S.pushedVol = a.volume; S.pushedMute = a.muted;
    }
    S.wasRunning = true;
}

void tab(App& a) {
    subNav("p25v", S.view, {"Now", "Calls", "Trunking", "Talkgroups", "Messages", "Follow"});
    if (S.view == 0) nowView(a); else if (S.view == 1) callsView(a); else if (S.view == 2) trunkingView(a); else if (S.view == 3) talkgroupView(a); else if (S.view == 4) messageView(a); else followView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    if (resultsClearButton("p25")) a.engine.p25().clearResults();
    const auto& tg = a.rx.p25.talkgroups;
    if (!ImGui::BeginTable("##p25l", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("TG"); ImGui::TableSetupColumn("Heard", ImGuiTableColumnFlags_WidthStretch);
    const auto& n = a.rx.p25.now;
    if (n.active && !n.group) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(pal::okGreen(), "%s", dect2::p25Id(n.src).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextColored(pal::okGreen(), "on air -> %s", dstOf(n).c_str());
    }
    for (const auto& g : tg) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextColored(g.active ? pal::okGreen() : ImGui::GetStyleColorVec4(ImGuiCol_Text), "%d", g.tg); ImGui::PopFont();
        ImGui::TableNextColumn();
        if (g.active) ImGui::TextColored(pal::okGreen(), "active, unit %s", dect2::p25Id(g.lastSrc).c_str());
        else ImGui::TextDisabled("%s  %u grants, %u calls", clock(g.lastWall).c_str(), g.grants, g.calls);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    wideCarriersTable(a, 0);
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::P25Telemetry& t = a.rx.p25;
    kvRow(a, "state", "%s", dect2::p25Summary(t).c_str());
    kvRow(a, "signal", "C4FM, symbol SNR %.1f dB, +3 deviation %.0f Hz, offset %+.0f Hz%s", t.snrDb, t.deviationHz, t.cfoHz, t.inverted ? ", inverted" : "");
    kvRow(a, "NAC", "%03X", t.nac < 0 ? 0 : t.nac);
    kvRow(a, "bit errors", "%.2f %% (NID, before the BCH)", t.berPct);
    kvRow(a, "frames", "%llu, %llu with a damaged NID", (unsigned long long)t.frames, (unsigned long long)t.nidBad);
    kvRow(a, "data units", "HDU %llu, LDU1 %llu, LDU2 %llu, TDU %llu, TDULC %llu, PDU %llu", (unsigned long long)t.hdu, (unsigned long long)t.ldu1, (unsigned long long)t.ldu2,
          (unsigned long long)t.tdu, (unsigned long long)t.tdulc, (unsigned long long)t.pdu);
    kvRow(a, "TSBK", "%llu good, %llu failed CRC", (unsigned long long)t.tsbkOk, (unsigned long long)t.tsbkBad);
    kvRow(a, "link control", "%llu good, %llu failed", (unsigned long long)t.lcOk, (unsigned long long)t.lcBad);
    kvRow(a, "encryption sync", "%llu good, %llu failed; header %llu good, %llu failed", (unsigned long long)t.esOk, (unsigned long long)t.esBad, (unsigned long long)t.hduOk, (unsigned long long)t.hduBad);
    kvRow(a, "voice", "%llu frames, %llu decoded%s, %llu calls joined late", (unsigned long long)t.voiceFrames, (unsigned long long)t.voiceDecoded, t.imbe ? "" : " (mbelib not found: click ? for the file names and folders)",
          (unsigned long long)t.lateEntries);
    if (t.imbe) kvRow(a, "mbelib", "%s", t.imbePath.c_str());
    kvRow(a, "input", "%.3f Msps, %.1f dBFS", t.inputRate / 1e6, t.levelDb);
    kvRow(a, "signal time", "%.1f s", t.timeSec);
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Sync", !on ? 0 : a.rx.p25.state >= 1 ? 1 : 0); flowNext(10 * gUi);
    lamp("Control", !on ? 0 : a.rx.p25.control ? 1 : 0); flowNext(10 * gUi);
    lamp("On air", !on ? 0 : a.rx.p25.state == 2 ? 1 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State"); ImGui::SameLine(0, 5 * gUi);
    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(!on ? (run ? "starting" : "stopped") : dect2::p25Summary(a.rx.p25).c_str()); ImGui::PopFont();
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "P25";
    if (live(a)) l2 = dect2::p25Summary(a.rx.p25);
}

} // namespace

// The trunking follower is shared by P25, DMR Tier III and NXDN Type C: the DMR and NXDN tabs tick it and shows the same panel (talkgroups, now talking, call log).
void trunkFollowTick(App& a) { trunkTick(a); }
void trunkFollowPanel(App& a) { followView(a); }

void trunkVoiceTick(App& v) {
    if (live(v)) appcodec::voiceCodecTick(v.rx.p25.phase2 ? "ambe2" : "imbe");   // the user's codec plug-in, as the mode's own tick does (Phase 2: AMBE+2)
    else if (liveDmr(v) || liveNxdn(v)) appcodec::voiceCodecTick("ambe2");
    if (!v.engine.running()) return;
    if (dect2::trunk::isAnalogTrunkFamily(v.family)) { analogSound(v); return; }   // FM: plays while it carries a call
    const uint64_t id = callOnRadioOf(v.rxId);
    if (v.family == 9) {   // DMR: decode and play the time slot of the call it was sent to
        int slot = 0;
        if (const dect2::trunk::CallRecord* r = id ? T.ctl.log.byId(id) : nullptr) if (r->slot == 0 || r->slot == 1) slot = r->slot + 1;
        v.engine.dmr().setListenSlot(slot);
    }
    pushSound(v);   // silent while it carries no call
}

void trunkVoicePanel(App& v) {
    using namespace dect2::trunk;
    size_t k = 0;
    while (k < T.voice.size() && T.voice[k].rxId != v.rxId) k++;
    if (k == T.voice.size()) { ImGui::TextDisabled("%s", tr("Not a voice radio any more: this tab closes.")); return; }   // stopped, closing
    const uint64_t id = T.ctl.follower.voiceCall((int)k);
    const CallRecord* r = id ? T.ctl.log.byId(id) : nullptr;
    char line[300];
    if (r) snprintf(line, sizeof line, tr("Voice for RX %d \xC2\xB7 TG %d \xC2\xB7 %.4f MHz"), v.voiceFor, r->tg, r->freqHz / 1e6);
    else snprintf(line, sizeof line, tr("Voice for RX %d \xC2\xB7 waiting for a call"), v.voiceFor);
    ImGui::PushFont(v.mono, 0);
    ImGui::TextColored(r ? pal::okGreen() : pal::grey(0.6f), "%s", line);
    ImGui::PopFont();
    if (r) if (const TgEntry* e = r->group ? T.ctl.tgs.find(r->tg) : nullptr) if (!e->alpha.empty()) { ImGui::SameLine(0, 8 * gUi); ImGui::TextUnformatted(e->alpha.c_str()); }
    ImGui::TextDisabled(tr("Radio: %s"), radioOf(v).c_str());
    if (!v.engine.running()) ImGui::TextColored(pal::warnAmber(), "%s", tr("starting"));
    if (ImGui::Button(tr("Unlink"))) { unlinkVoice(k, fmtRx(tr("RX %d is no longer a voice radio: its tab is closed."), v.rxId), Unlink::Close, false); return; }
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", fmtRx(tr("This receiver only follows calls for RX %d. The talkgroups, the call log and the settings are on the Follow tab of RX %d."), v.voiceFor, v.voiceFor).c_str());
    ImGui::PopTextWrapPos();
}

void trunkVoiceFrame() {
    using namespace dect2::trunk;
    recordWatch();   // whatever ended the recorded call, its recording stops
    if (T.voice.empty()) return;
    App* c = receiverById(T.ctlRx);
    RxState cs;
    if (c) cs = stateOf(*c);
    const double now = nowSec();
    for (size_t k = T.voice.size(); k-- > 0;) {
        Trunk::VoiceRadio& vr = T.voice[k];
        const int id = vr.rxId;
        App* v = receiverById(id);
        RxState vs;
        if (v) vs = stateOf(*v);
        if (v && vs.running && vs.claimed && !vs.radioLost) vr.ran = true;
        switch (voiceFate(v ? &vs : nullptr, c ? &cs : nullptr, T.linkFamily, T.linkSub, vr.ran || now - vr.linkedSec > 15)) {
        case VoiceFate::Keep: break;
        case VoiceFate::AllClose:
            unlinkAll(c ? fmtRx(tr("RX %d switched to another mode: its voice radios are unlinked and closed."), c->rxId)
                        : std::string(tr("The receiver on the control channel was closed: its voice radios are closed too.")), Unlink::Close, true);
            return;
        case VoiceFate::Gone:
            unlinkVoice(k, fmtRx(tr("The voice tab RX %d was closed: RX %d follows calls with its own radio again."), id, T.ctlRx), Unlink::Gone, true);
            break;
        case VoiceFate::UnlinkKeep:
            unlinkVoice(k, fmtRx(tr("RX %d was switched to another mode: it is no longer a voice radio."), id), Unlink::Keep, true);
            break;
        case VoiceFate::UnlinkClose: {
            std::string w;
            if (vs.radioLost || (vs.running && !vs.claimed)) {
                char why[700];
                snprintf(why, sizeof why, tr("RX %d lost its radio (%s): the call on it ended, it is no longer a voice radio and its tab closes. RX %d stays on the control channel."),
                         id, radioOf(*v).c_str(), T.ctlRx);
                w = why;
            } else w = fmtRx(tr("RX %d was stopped: it is no longer a voice radio and its tab closes. RX %d stays on the control channel."), id, T.ctlRx);
            if (!v->startErr.empty()) w += " " + v->startErr;
            unlinkVoice(k, w, Unlink::Close, true);
            break;
        }
        }
    }
    // following off (the switch, or the scanner left the system), or the control receiver stopped: the radios stay linked and silent
    if (!T.voice.empty() && (!T.on || !(c && c->engine.running()))) idleVoice(T.on ? "the control receiver stopped" : "following switched off");
}

void trunkVoiceShutdown() {
    while (!T.voice.empty()) { T.ctl.dropVoiceRadio((int)T.voice.size() - 1, "program closed"); T.voice.pop_back(); }   // ends their calls now
    applyActions(nullptr);   // their recordings stop
    recordWatch();
    T.heardBefore = 0;
}

// The scanner on a trunked system's entry: follow its calls while it is there. enter: a new system, so the site and the follower start
// again (the call log and the talkgroups stay) and the new receiver's message counters are taken from the start.
void trunkScanFollow(App& a, bool on, bool enter) {
    static bool userOn = false, held = false;   // the user's own setting, given back when the scanner leaves trunked systems
    if (on && !held) { userOn = T.on; held = true; }
    if (enter) {   // a new system: what the receiver still holds from the last one is not this system's
        T.ctl.sys.clear();
        T.ctl.follower.reset();
        applyActions(nullptr);   // the recordings of the calls it dropped stop
        soundBack();             // and the sound comes back from a voice radio
        Trunk::Seen& sn = T.seen[a.rxId];
        sn.tsbk = a.rx.p25.tsbks.empty() ? 0 : a.rx.p25.tsbks.back().seq;
        sn.csbk = a.rx.dmr.csbks.empty() ? 0 : a.rx.dmr.csbks.back().seq;
        sn.nxdn = a.rx.nxdn.grants.empty() ? 0 : a.rx.nxdn.grants.back().seq;
        sn.nxdnCac = a.rx.nxdn.cacOk;
        sn.edacs = a.rx.edacs.msgs.empty() ? 0 : a.rx.edacs.msgs.back().seq;
        sn.ltr = a.rx.ltr.words.empty() ? 0 : a.rx.ltr.words.back().seq;
        sn.mpt = a.rx.mpt1327.msgs.empty() ? 0 : a.rx.mpt1327.msgs.back().seq;
        T.ctl.edacs.reset();
        T.ctl.ltr.reset();
    }
    if (!on && held) { if (T.on) T.on = userOn; held = false; return; }   // a user who switched it off meanwhile keeps it off
    T.on = on;
}
// A call the follower follows right now, and what it is ("talkgroup 77 Fire Dispatch"); false when none.
bool trunkCallNow(std::string* what) {
    for (const dect2::trunk::CallRecord* r : T.ctl.nowTalking()) {
        if (!r->followed) continue;
        if (what) {
            const dect2::trunk::TgEntry* e = r->group ? T.ctl.tgs.find(r->tg) : nullptr;
            *what = std::string(r->group ? "talkgroup " : "unit ") + std::to_string(r->tg) + (e && !e->alpha.empty() ? " " + e->alpha : "");
        }
        return true;
    }
    return false;
}

extern const ModeUi kP25Ui;
const ModeUi kP25Ui = {
    .sideTitle = "TALKGROUPS",
    .tabName = "P25",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};

void p25DevView(int v) { S.view = v; }   // dev: the sub-view the P25 tab starts on (screenshots)
