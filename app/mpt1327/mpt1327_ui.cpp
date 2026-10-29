// MPT1327 trunking screens: lock, good and bad codewords, the system identity, the control message list and the channel table, and the
// Follow tab of the shared trunking follower (app/p25/p25_ui.cpp: base and spacing, the channel list, talkgroups, call log, voice radios).
#include "app.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include <cstdio>
#include <string>

namespace {

struct State { int view = 0; };
State S;

bool live(const App& a) { return a.rx.standard == 128; }   // family 127, stdMode 129, active standard 128

void messageTable(const dect2::MptTelemetry& t, float height) {
    if (!ImGui::BeginTable("mpt_msgs", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV, ImVec2(0, height))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn("Caller", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("Called", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 56);
    ImGui::TableSetupColumn("Detail");
    ImGui::TableHeadersRow();
    for (size_t i = t.rows.size(); i-- > 0;) {   // newest first
        const dect2::MptMessageRow& r = t.rows[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%.1f s", r.t);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.type.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.caller.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.called.c_str());
        ImGui::TableNextColumn();
        if (r.channel >= 0) ImGui::Text("%d", r.channel); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(r.detail.c_str());
    }
    ImGui::EndTable();
}

void channelTable(const dect2::MptTelemetry& t) {
    if (!ImGui::BeginTable("mpt_chans", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Channel", ImGuiTableColumnFlags_WidthFixed, 60);
    ImGui::TableSetupColumn("Use", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("Seen", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("Last called");
    ImGui::TableSetupColumn("Last caller");
    ImGui::TableHeadersRow();
    for (const dect2::MptChannelRow& c : t.channels) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", c.channel);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.controlChannel ? "control channel" : "traffic (GTC)");
        ImGui::TableNextColumn(); ImGui::Text("%u", (unsigned)c.count);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.called.empty() ? "-" : c.called.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.caller.empty() ? "-" : c.caller.c_str());
    }
    ImGui::EndTable();
}

void controlView(App& a) {
    if (!live(a)) {
        ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to an MPT1327 control channel (VHF / UHF, 12.5 or 25 kHz FM) and start the receiver");
        return;
    }
    const dect2::MptTelemetry& t = a.rx.mpt1327;
    ImGui::TextUnformatted("MPT1327 trunking: 1200 bit/s FFSK control channel; calls are followed on the Follow tab");
    ImGui::Spacing();
    ImGui::Text("Lock: %s", t.locked ? "locked" : "searching");
    if (t.sysId >= 0) ImGui::Text("System identity: %d", t.sysId);
    else ImGui::TextDisabled("System identity: -");
    ImGui::Text("Codewords good: %llu, bad: %llu", (unsigned long long)t.good, (unsigned long long)t.bad);
    ImGui::Text("Messages: %llu", (unsigned long long)t.messages);
    ImGui::Text("Level: %.0f dBFS", t.levelDb);
    ImGui::Spacing();
    ImGui::SeparatorText("Channel table");
    if (t.channels.empty()) ImGui::TextDisabled("no channel numbers seen yet");
    else channelTable(t);
    ImGui::Spacing();
    if (t.voice.carrier && t.synt) ImGui::Text("Traffic channel: %llu CLEAR, %llu MAINT", (unsigned long long)t.clears, (unsigned long long)t.maints);
    ImGui::TextDisabled("Input %.0f Hz, centre %.4f MHz.", t.inputRate, t.centerHz / 1e6);
}

void tab(App& a) {
    subNav("mptv", S.view, {"Control channel", "Follow"});
    if (S.view == 0) controlView(a); else trunkFollowPanel(a);
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::MptTelemetry& t = a.rx.mpt1327;
    ImGui::Text("%s", dect2::mpt1327Summary(t).c_str());
    ImGui::Text("good %llu, bad %llu, level %.1f dBFS", (unsigned long long)t.good, (unsigned long long)t.bad, t.levelDb);
}

void list(App& a) {
    if (!live(a)) return;
    const dect2::MptTelemetry& t = a.rx.mpt1327;
    if (t.rows.empty()) { ImGui::TextDisabled("no messages yet"); return; }
    messageTable(t, ImGui::GetContentRegionAvail().y);
}

void tick(App& a) { trunkFollowTick(a); }   // the shared follower (app/p25/p25_ui.cpp); also the traffic channel's sound

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::mpt1327Summary(a.rx.mpt1327).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "MPT1327 trunking";
    if (live(a)) l2 = dect2::mpt1327Summary(a.rx.mpt1327);
}

} // namespace

extern const ModeUi kMpt1327Ui;
const ModeUi kMpt1327Ui = {
    .sideTitle = "MESSAGES",
    .tabName = "MPT1327",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
