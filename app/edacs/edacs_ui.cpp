// EDACS trunking screens: the control channel (lock, frames, the BCH and vote counts, the system, the messages in words) and the Follow tab
// of the shared trunking follower (app/p25/p25_ui.cpp: the LCN table, talkgroups, call log, voice radios). Receive only.
#include "app.h"
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/edacs_tel.h"
#include <cstdio>
#include <string>

namespace {

struct State { int view = 0; };
State S;

bool live(const App& a) { return a.rx.standard == 131; }   // family 130, engine standard 132, active standard 131

void controlView(App& a) {
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to an EDACS control channel (9600 bit/s, or 4800 for EDACS NB) and start the receiver");
        return;
    }
    const dect2::EdacsTelemetry& t = a.rx.edacs;
    ImGui::TextUnformatted("EDACS trunking: control channel; calls are followed on the Follow tab");
    ImGui::Spacing();
    ImGui::Text("Lock: %s", t.locked ? "yes" : "no");
    ImGui::Text("Good frames: %llu   Bad frames: %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    ImGui::Text("Messages: %llu passed the BCH check, %llu after correcting it, %llu refused", (unsigned long long)t.bchOk, (unsigned long long)t.bchFixed, (unsigned long long)t.msgBad);
    ImGui::Text("Level: %.0f dBFS, data swing %.0f Hz, %s polarity%s", t.levelDb, t.deviationHz, t.inverted ? "inverted" : "normal", t.narrow ? ", narrowband 4800 bit/s" : "");
    if (t.voice.carrier && !t.locked) ImGui::Text("Working channel: %s%s", t.provoice ? "ProVoice (digital voice, no decoder)" : "FM voice", t.lsd ? ", low-speed data" : "");
    ImGui::Spacing();
    ImGui::TextUnformatted("Data words (28 bits)");
    if (ImGui::BeginTable("edword", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Word");
        ImGui::TableSetupColumn("Count");
        ImGui::TableHeadersRow();
        for (const dect2::EdacsWordCount& w : t.words) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%07X", w.word);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)w.count);
        }
        ImGui::EndTable();
    }
    if (!t.dataValid) ImGui::TextDisabled("listening: no frame decoded yet");
}

void tab(App& a) {
    subNav("edacsv", S.view, {"Control channel", "Follow"});
    if (S.view == 0) controlView(a); else trunkFollowPanel(a);
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::EdacsTelemetry& t = a.rx.edacs;
    ImGui::Text("%s", dect2::edacsSummary(t).c_str());
    ImGui::Text("good %llu, bad %llu, level %.1f dBFS", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.levelDb);
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::EdacsTelemetry& t = a.rx.edacs;
    const dect2::edacs::Mode md = trunkEdacsMode() == 1 ? dect2::edacs::Mode::Extended : dect2::edacs::Mode::Standard;
    if (ImGui::BeginTable("edmsg", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Message");
        ImGui::TableSetupColumn("Words");
        ImGui::TableSetupColumn("Copy errors");
        ImGui::TableSetupColumn("Time (s)");
        ImGui::TableHeadersRow();
        for (const dect2::EdacsFrame& f : t.frames) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (f.okA && f.okB) ImGui::TextUnformatted(dect2::edacs::describe(dect2::edacs::parse(f.msgA, f.msgB, md, trunkEdacsEsk() ? dect2::edacs::kEsk : 0), md).c_str());
            else ImGui::TextDisabled("BCH check failed");
            ImGui::TableNextColumn(); ImGui::Text("%07X %07X", f.msgA, f.msgB);
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", f.errA, f.errB);
            ImGui::TableNextColumn(); ImGui::Text("%.1f", f.timeSec);
        }
        ImGui::EndTable();
    }
}

void tick(App& a) { trunkFollowTick(a); }   // the shared follower (app/p25/p25_ui.cpp); also the working channel's sound

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::edacsSummary(a.rx.edacs).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "EDACS trunking";
    if (live(a)) l2 = dect2::edacsSummary(a.rx.edacs);
}

} // namespace

extern const ModeUi kEdacsUi;
const ModeUi kEdacsUi = {
    .sideTitle = "FRAMES",
    .tabName = "EDACS",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
