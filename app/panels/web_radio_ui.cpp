// View > Internet radio: search the radio-browser.info directory (a free, community station list) and play a station's stream through
// the program's sound output (so "Record audio", "Stream audio" and a virtual audio cable get it too). core/src/audio/web_radio.cpp plays.
#include "../app.h"
#include "dect2/audio/web_radio.h"
#include "dect2/automation/json_mini.h"
#include <cstdio>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Station { std::string name, url, country, codec, tags; int kbps = 0; };

struct State {
    char query[96] = "";
    std::vector<Station> found;
    std::future<std::vector<Station>> pending;
    std::string note;
};
State S;

std::string urlEncode(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}

// the directory's answer: a JSON array of stations; only the ones with an http(s) stream address are kept
std::vector<Station> parseStations(const std::string& text) {
    namespace js = dect2::automation::json;
    std::vector<Station> v;
    js::Value root; std::string err;
    if (!js::parse(text, root, err) || root.type != js::Value::Arr) return v;
    for (const js::Value& e : root.a) {
        Station s;
        s.name = e.str("name"); s.url = e.str("url_resolved", e.str("url"));
        s.country = e.str("countrycode"); s.codec = e.str("codec"); s.tags = e.str("tags");
        s.kbps = (int)e.num("bitrate");
        if (!s.name.empty() && dect2::WebRadio::urlOk(s.url)) v.push_back(std::move(s));
    }
    return v;
}

std::vector<Station> search(std::string q) {
    const std::string path = plat::cacheDir() + "/saurcom_radio_browser.json";
    const std::string url = "https://all.api.radio-browser.info/json/stations/search?hidebroken=true&order=clickcount&reverse=true&limit=60&name=" + urlEncode(q);
    std::vector<Station> v;
    if (plat::fetchUrl(url, path, "EyeOfSaurCom")) {
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss; ss << f.rdbuf();
        v = parseStations(ss.str());
    }
    std::remove(path.c_str());
    return v;
}

} // namespace

void webRadioWindow(App& a) {
    if (!a.webRadioOpen) return;
    dect2::WebRadio& R = dect2::WebRadio::get();
    R.setVolume(a.volume);
    R.setMuted(a.muted);
    ImGui::SetNextWindowSize(ImVec2(560 * gUi, 420 * gUi), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Internet radio"), &a.webRadioOpen, ImGuiWindowFlags_NoCollapse)) { ImGui::End(); return; }
    if (S.pending.valid() && S.pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        S.found = S.pending.get();
        S.note = S.found.empty() ? tr("No station found (or the directory could not be reached).") : "";
    }
    ImGui::SetNextItemWidth(-90 * gUi);
    const bool enter = ImGui::InputTextWithHint("##q", tr("station name, e.g. jazz, BBC, news"), S.query, sizeof S.query, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(S.pending.valid());
    if ((ImGui::Button(tr("Search"), ImVec2(-1, 0)) || enter) && !S.pending.valid()) S.pending = std::async(std::launch::async, search, std::string(S.query));
    ImGui::EndDisabled();
    const dect2::WebRadioStats st = R.stats();
    if (st.playing || !st.codec.empty()) {
        if (ImGui::SmallButton(tr("Stop"))) R.stop();
        ImGui::SameLine();
        ImGui::TextColored(st.playing ? pal::okGreen() : pal::grey(), "%s  %s %d Hz  %.0f s", st.url.c_str(), st.codec.c_str(), st.rate, st.seconds);
    }
    if (!st.error.empty()) ImGui::TextColored(pal::warnAmber(), "%s", st.error.c_str());
    if (!S.note.empty()) ImGui::TextDisabled("%s", S.note.c_str());
    if (ImGui::BeginTable("##stations", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Station"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Country")); ImGui::TableSetupColumn(tr("Codec")); ImGui::TableSetupColumn("kbit/s");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < S.found.size(); i++) {
            const Station& s = S.found[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)i);
            if (ImGui::Selectable(s.name.c_str(), st.url == s.url && st.playing, ImGuiSelectableFlags_SpanAllColumns)) {
                std::string err;
                if (!R.play(s.url, err)) S.note = err;
            }
            if (ImGui::IsItemHovered() && !s.tags.empty()) ImGui::SetTooltip("%s\n%s", s.tags.c_str(), s.url.c_str());
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.country.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.codec.c_str());
            ImGui::TableNextColumn(); if (s.kbps > 0) ImGui::Text("%d", s.kbps);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}
