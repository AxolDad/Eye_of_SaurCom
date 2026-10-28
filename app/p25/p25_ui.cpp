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
    uint64_t tsbkSeen = 0, csbkSeen = 0, nxdnSeen = 0, nxdnCac = 0;
    std::string dmrChannels;             // "LCN=MHz" pairs for Tier III (pref "trunkDmrChannels")
    std::string nxdnChannels;            // "channel=MHz" pairs for NXDN Type C (pref "trunkNxdnChannels")
    int forcedSel = 0;
    uint64_t recCall = 0;
    std::string status;
    char csvPath[300] = "";
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
int applyDmrChannels(const std::string& text) { return applyChannels(text, 4095, [](int n, double hz) { T.ctl.dmr.setChannel(n, hz); }); }
int applyNxdnChannels(const std::string& text) { return applyChannels(text, 1023, [](int n, double hz) { T.ctl.nxdn.setChannel(n, hz); }); }

void trunkTick(App& a) {
    if (!T.loaded) {
        T.loaded = true;
        if (std::filesystem::exists(std::filesystem::u8path(tgFile()))) tgLoad(tgFile(), false);
        T.dmrChannels = plat::prefs().getS("trunkDmrChannels", "");
        applyDmrChannels(T.dmrChannels);
        T.nxdnChannels = plat::prefs().getS("trunkNxdnChannels", "");
        applyNxdnChannels(T.nxdnChannels);
    }
    const bool p25 = live(a), dmr = liveDmr(a), nxdn = liveNxdn(a);
    if (!T.on || !(p25 || dmr || nxdn)) return;
    using namespace dect2::trunk;
    const double now = nowSec();
    const dect2::P25Telemetry& t = a.rx.p25;
    // a restarted receiver counts its messages from 1 again
    if (!t.tsbks.empty() && t.tsbks.back().seq < T.tsbkSeen) T.tsbkSeen = 0;
    if (!a.rx.dmr.csbks.empty() && a.rx.dmr.csbks.back().seq < T.csbkSeen) T.csbkSeen = 0;
    if (!a.rx.nxdn.grants.empty() && a.rx.nxdn.grants.back().seq < T.nxdnSeen) T.nxdnSeen = 0;
    if (p25) for (const auto& r : t.tsbks) {
        if (r.seq <= T.tsbkSeen) continue;
        T.tsbkSeen = r.seq;
        dect2::p25::Tsbk k; k.opcode = r.opcode; k.mfid = r.mfid; k.args = r.args;
        T.ctl.onTsbk(k, r.nac, now);
    }
    if (dmr) for (const auto& r : a.rx.dmr.csbks) {
        if (r.seq <= T.csbkSeen) continue;
        T.csbkSeen = r.seq;
        T.ctl.onDmrCsbk(r.opcode, r.fid, r.data, now);
    }
    if (nxdn) {
        // a Type C control channel is on air when new control channel (CAC) frames decode; site info alone is kept from earlier
        if (a.rx.nxdn.cacOk < T.nxdnCac) T.nxdnCac = 0;
        if (a.rx.nxdn.cacOk > T.nxdnCac) { T.nxdnCac = a.rx.nxdn.cacOk; T.ctl.onControlSeen(now); }
        for (const auto& g : a.rx.nxdn.grants) {
            if (g.seq <= T.nxdnSeen) continue;
            T.nxdnSeen = g.seq;
            T.ctl.onNxdnAssignment(g.callType, g.src, g.dst, g.channel, g.ccOption, now);
        }
    }
    T.ctl.setCapture(radioCenterMhz(a) * 1e6, p25 && wideActive(a) ? t.inputRate : 0);   // DMR, NXDN: the radio retunes for each call
    if (const uint64_t id = T.ctl.follower.audibleCall()) {   // on the call's channel: voice heard keeps the call (and the radio) there
        const CallRecord* r = T.ctl.log.byId(id);
        bool voice = false;
        // only this call's own voice counts: the channel may already carry another talkgroup's call (its grant was not heard here)
        auto ours = [&](long long dst) { return r && (dst < 0 || dst == r->tg); };
        if (p25) voice = t.now.active && t.now.kind == 0 && ours(t.now.dst);
        else if (dmr) voice = r && r->slot >= 0 && r->slot < 2 && a.rx.dmr.slot[r->slot].active && a.rx.dmr.slot[r->slot].state == 1 && ours(a.rx.dmr.slot[r->slot].dst ? (long long)a.rx.dmr.slot[r->slot].dst : -1);   // 0: not decoded yet
        else voice = a.rx.nxdn.now.active && a.rx.nxdn.now.kind == 0 && ours(a.rx.nxdn.now.dst);
        if (voice && !(p25 && wideActive(a))) T.ctl.onVoice(id, now);
    }
    T.ctl.tick(now);
    for (const Action& ac : T.ctl.takeActions()) {
        switch (ac.kind) {
        case Action::Retune: tuneFreq(a, ac.freqHz / 1e6); break;
        case Action::Notice: T.ctl.messages.push_back(ac.text); break;
        case Action::RecordStart: {
            dect2::AudioRecorder& rec = dect2::AudioRecorder::instance();
            if ((p25 && !dect2::p25ImbeAvailable()) || rec.active()) break;   // no vocoder: there is no sound to save
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
            if (T.recCall == ac.callId) { dect2::AudioRecorder::instance().stop(); T.recCall = 0; }
            break;
        default: break;
        }
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

void followView(App& a) {
    using namespace dect2::trunk;
    ImGui::Checkbox("Follow calls", &T.on);
    ImGui::SameLine(0, 10 * gUi);
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Follows the voice grants of a P25 control channel. Inside a wide capture every channel is decoded and the sound moves to the call you should hear;\n"
                          "outside it the one radio leaves the control channel for the call and comes back. Encrypted calls are logged as encrypted and never followed.\n"
                          "DMR Tier III and NXDN Type C grants are followed the same way, with your channel table.\n"
                          "Sound needs your own mbelib; the talkgroups and the call log work without it. See docs/trunking.md.");
    if (!T.on) { ImGui::TextDisabled("%s", live(a) || liveDmr(a) || liveNxdn(a) ? "off: the control channel is only decoded, nothing is followed" : "start the receiver first"); }
    if (liveDmr(a)) {
        ImGui::TextDisabled("DMR Tier III: the grants give channel numbers; enter each channel's frequency (LCN=MHz, e.g. 1=451.0125 2=451.0375)");
        char b[512]; snprintf(b, sizeof b, "%s", T.dmrChannels.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##dmrlcn", b, sizeof b)) T.dmrChannels = b;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            T.ctl.dmr = dect2::trunk::DmrModel();
            const int n = applyDmrChannels(T.dmrChannels);
            plat::prefs().setS("trunkDmrChannels", T.dmrChannels);
            T.status = std::to_string(n) + " DMR channels set";
        }
        ImGui::TextDisabled("%llu Tier III grants seen", (unsigned long long)T.ctl.dmr.grants());
    }
    if (liveNxdn(a)) {
        ImGui::TextDisabled("NXDN Type C: the grants give channel numbers; enter each channel's frequency (channel=MHz, e.g. 1=451.0125 2=451.0250)");
        char b[512]; snprintf(b, sizeof b, "%s", T.nxdnChannels.c_str());
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##nxdnch", b, sizeof b)) T.nxdnChannels = b;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            T.ctl.nxdn = dect2::trunk::NxdnModel();
            const int n = applyNxdnChannels(T.nxdnChannels);
            plat::prefs().setS("trunkNxdnChannels", T.nxdnChannels);
            T.status = std::to_string(n) + " NXDN channels set";
        }
        ImGui::TextDisabled("%llu NXDN channel assignments seen", (unsigned long long)T.ctl.nxdn.grants());
    }
    const double now = nowSec();
    const ControllerStatus st = T.ctl.status(now);
    kvRow(a, "state", "%s%s", followStateName(st.state), st.controlLocked ? "" : " (no control channel message lately)");
    kvRow(a, "control channel", "%llu good, %llu bad (%.0f %% ok), %u calls on air, %llu not followed",
          (unsigned long long)st.tsbkGood, (unsigned long long)st.tsbkBad, st.crcOkPct, (unsigned)st.activeCalls, (unsigned long long)st.missed);
    const SiteInfo& si = T.ctl.sys.site();
    if (si.valid) kvRow(a, "site", "WACN %05X system %03X RFSS %d site %d NAC %03X", si.wacn < 0 ? 0 : si.wacn, si.sysId < 0 ? 0 : si.sysId, si.rfss, si.site, si.nac < 0 ? 0 : si.nac);
    if (wideActive(a)) ImGui::TextDisabled("wide capture: calls inside the span are followed by the carrier decoders (up to %d at once)", T.ctl.follower.config().maxSlices);
    else ImGui::TextDisabled("single channel: the radio leaves the control channel for each call (turn on whole-span decoding to follow inside the capture)");
    // asked for often with several radios attached: say plainly what works today (docs/trunking.md, "Several radios")
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Voice on a second radio is not supported yet: this receiver follows calls with its own radio. "
                                 "With a wide-band radio (HackRF), turn on whole-span decoding so the control channel and the voice channels are received together."));
    ImGui::PopTextWrapPos();
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
            ImGui::TableNextColumn(); ImGui::Text("%s%d", r->group ? "" : "unit ", r->tg);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e ? e->alpha.c_str() : "");
            ImGui::TableNextColumn(); ImGui::Text("%d", r->src);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(mhz(r->freqHz).c_str());
            ImGui::TableNextColumn();
            if (r->encrypted) ImGui::TextColored(pal::warnAmber(), "encrypted: not followed");
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
    if (ImGui::Button("Add heard")) { for (const auto& g : a.rx.p25.talkgroups) T.ctl.tgs.upsert(g.tg); T.dirty = true; }
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
            ImGui::TableNextColumn(); ImGui::Text("%d", r.tg);
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
    if (live(a)) appcodec::voiceCodecTick("imbe");   // the user's codec plug-in (docs/CODECS.md), loaded whichever tab is shown
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

// The scanner on a trunked system's entry: follow its calls while it is there. enter: a new system, so the site and the follower start
// again (the call log and the talkgroups stay) and the new receiver's message counters are taken from the start.
void trunkScanFollow(App& a, bool on, bool enter) {
    static bool userOn = false, held = false;   // the user's own setting, given back when the scanner leaves trunked systems
    if (on && !held) { userOn = T.on; held = true; }
    if (enter) {   // a new system: what the receiver still holds from the last one is not this system's
        T.ctl.sys.clear();
        T.ctl.follower.reset();
        T.tsbkSeen = a.rx.p25.tsbks.empty() ? 0 : a.rx.p25.tsbks.back().seq;
        T.csbkSeen = a.rx.dmr.csbks.empty() ? 0 : a.rx.dmr.csbks.back().seq;
        T.nxdnSeen = a.rx.nxdn.grants.empty() ? 0 : a.rx.nxdn.grants.back().seq;
        T.nxdnCac = a.rx.nxdn.cacOk;
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
