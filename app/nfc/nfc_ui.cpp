// NFC / RFID sniffer screens: the frames between a reader and a card as they pass, the cards seen (UID, ATQA, SAK, the type the public
// tables give), the reader field and the decoder's settings, and a privacy note. Receive only: nothing is sent and no card is emulated.
#include "app.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/nfc/nfc_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

namespace {

struct State {
    int view = 0;                // 0 frames, 1 cards, 2 field and settings
    int dir = 0;                 // frame filter: 0 all, 1 reader, 2 card
    int tech = 0;                // 0 all, 1 A, 2 B, 3 F, 4 V
    bool hideBad = false;
};
State S;

bool live(const App& a) { return a.rx.standard == 149; }   // family 148, engine standard 150, active standard 149

std::string hhmmss(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    localtime_s(&g, &tt);
#else
    localtime_r(&tt, &g);
#endif
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", &g);
    return b;
}

std::string ago(int64_t t) {
    if (t <= 0) return "-";
    const long d = (long)(std::time(nullptr) - t);
    char b[32];
    if (d < 60) snprintf(b, sizeof b, "%ld s", std::max(0L, d));
    else if (d < 3600) snprintf(b, sizeof b, "%ld min", d / 60);
    else snprintf(b, sizeof b, "%ld h", d / 3600);
    return b;
}

bool frameShown(const dect2::NfcFrame& f) {
    if (S.dir == 1 && !f.poll) return false;
    if (S.dir == 2 && f.poll) return false;
    if (S.tech && (int)f.tech != S.tech) return false;
    if (S.hideBad && (f.parityError || f.crcError)) return false;
    return true;
}

std::string flagText(const dect2::NfcFrame& f) {
    std::string s;
    if (f.encrypted) s += "encrypted ";
    if (f.parityError) s += "parity ";
    if (f.crcError) s += "CRC ";
    if (f.truncated) s += "cut ";
    if (!s.empty()) s.pop_back();
    return s;
}

void privacyNote() {
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Privacy: this listens to the exchange between a reader and a card near the antenna, to check what your own cards and "
                        "readers give away. Only what passes in the clear is shown; what the two encrypt is marked and not decoded. Nothing is sent, "
                        "no card is emulated, nothing is looked up or sent anywhere. A UID identifies a card and often its holder: do not record or "
                        "share other people's.");
    ImGui::PopTextWrapPos();
}

void framesView(App& a) {
    const dect2::NfcTelemetry& t = a.rx.nfc;
    ImGui::SetNextItemWidth(110 * gUi);
    const char* dirs[] = {"Both ways", "Reader", "Card"};
    ImGui::Combo("##nfcdir", &S.dir, dirs, 3);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    const char* techs[] = {"All", "NFC-A", "NFC-B", "NFC-F", "NFC-V"};
    ImGui::Combo("##nfctech", &S.tech, techs, 5);
    ImGui::SameLine();
    ImGui::Checkbox("Hide damaged", &S.hideBad);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave out frames with a parity or CRC error.");
    ImGui::SameLine(0, 10 * gUi);
    if (ImGui::SmallButton("Copy")) {
        std::string txt;
        for (const auto& f : t.frames) {
            if (!frameShown(f)) continue;
            char b[128];
            snprintf(b, sizeof b, "%s\t%.6f\t%s\t%s\t%d kbit/s\t", hhmmss(f.wallTime).c_str(), f.sec, f.poll ? "reader" : "card", dect2::nfc::techName(f.tech), f.rateKbps);
            txt += b;
            txt += dect2::nfc::hex(f.bytes.data(), f.bytes.size()) + "\t" + f.name;
            const std::string fl = flagText(f);
            if (!fl.empty()) txt += "\t" + fl;
            txt += "\n";
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the shown frames, one per line: time, direction, technology, rate, bytes, meaning.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) a.engine.nfc().clearResults();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget the frames and the cards.");
    if (!ImGui::BeginTable("##nfcframes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time"); ImGui::TableSetupColumn("Who"); ImGui::TableSetupColumn("Tech"); ImGui::TableSetupColumn("Meaning");
    ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Flags");
    ImGui::TableHeadersRow();
    for (const auto& f : t.frames) {
        if (!frameShown(f)) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%s  %.4f", hhmmss(f.wallTime).c_str(), f.sec);
        ImGui::TableNextColumn();
        if (f.poll) ImGui::TextColored(pal::accent(), "reader >"); else ImGui::TextUnformatted("< card");
        ImGui::TableNextColumn(); ImGui::Text("%s %d", dect2::nfc::techName(f.tech), f.rateKbps);
        ImGui::TableNextColumn();
        if (f.encrypted) ImGui::TextDisabled("%s", f.name.c_str()); else ImGui::TextUnformatted(f.name.c_str());
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0);
        ImGui::TextUnformatted(dect2::nfc::hex(f.bytes.data(), f.bytes.size()).c_str());
        if (f.bits) { ImGui::SameLine(); ImGui::TextDisabled("(%d bits)", f.bits); }
        ImGui::PopFont();
        ImGui::TableNextColumn();
        const std::string fl = flagText(f);
        if (f.parityError || f.crcError || f.truncated) ImGui::TextColored(pal::warnAmber(), "%s", fl.c_str()); else ImGui::TextDisabled("%s", fl.c_str());
    }
    ImGui::EndTable();
    if (t.frames.empty()) ImGui::TextDisabled("nothing heard yet: bring a card to a reader within reach of the antenna");
}

void cardsView(App& a) {
    const dect2::NfcTelemetry& t = a.rx.nfc;
    if (t.cards.empty()) { ImGui::TextDisabled("no card selected by a reader yet"); return; }
    if (ImGui::SmallButton("Copy")) {
        std::string txt;
        for (const auto& c : t.cards) {
            char b[96];
            snprintf(b, sizeof b, "\t%s\tATQA %02X %02X\tSAK %02X\t", dect2::nfc::techName(c.tech), c.atqa[0], c.atqa[1], c.sak);
            txt += dect2::nfc::hex(c.uid.data(), c.uid.size()) + b + c.type + "\t" + c.manufacturer + "\n";
        }
        ImGui::SetClipboardText(txt.c_str());
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy the cards, one per line: UID, technology, ATQA, SAK, type, manufacturer.");
    if (!ImGui::BeginTable("##nfccards", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV,
                           ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("UID"); ImGui::TableSetupColumn("Tech"); ImGui::TableSetupColumn("ATQA"); ImGui::TableSetupColumn("SAK");
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Made by"); ImGui::TableSetupColumn("Selected"); ImGui::TableSetupColumn("Last");
    ImGui::TableHeadersRow();
    for (const auto& c : t.cards) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::nfc::hex(c.uid.data(), c.uid.size()).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(dect2::nfc::techName(c.tech));
        ImGui::TableNextColumn();
        if (c.tech == dect2::nfc::TechA) ImGui::Text("%02X %02X", c.atqa[0], c.atqa[1]); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        if (c.tech == dect2::nfc::TechA) ImGui::Text("%02X", c.sak); else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(c.type.empty() ? "-" : c.type.c_str());
        if (c.iso14443_4) { ImGui::SameLine(); ImGui::TextDisabled("ISO 14443-4"); }
        if (c.encryptedSeen) { ImGui::SameLine(); ImGui::TextDisabled("(encrypted exchange seen)"); }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.manufacturer.empty() ? "-" : c.manufacturer.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.selections);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(c.lastWall).c_str());
    }
    ImGui::EndTable();
}

void fieldView(App& a) {
    const dect2::NfcTelemetry& t = a.rx.nfc;
    ImGui::Text("Reader field: %s", t.fieldOn ? "present" : "none");
    ImGui::SameLine(); ImGui::TextDisabled("(came up %llu times)", (unsigned long long)t.fieldOnCount);
    ImGui::Text("Input level: %.1f dBFS", t.levelDb);
    ImGui::Text("Frames: %llu from readers, %llu from cards; %llu damaged", (unsigned long long)t.pollFrames, (unsigned long long)t.listenFrames, (unsigned long long)t.blocksBad);
    ImGui::Text("By technology: NFC-A %llu, NFC-B %llu, NFC-F %llu, NFC-V %llu", (unsigned long long)t.framesA, (unsigned long long)t.framesB, (unsigned long long)t.framesF, (unsigned long long)t.framesV);
    ImGui::Spacing();
    ImGui::TextUnformatted("Decoder");
    int mask = t.techMask;
    bool changed = false;
    const char* names[] = {"NFC-A (ISO 14443-A: MIFARE, NTAG, most cards)", "NFC-B (ISO 14443-B: passports, some ID cards)", "NFC-F (FeliCa)", "NFC-V (ISO 15693 tags)"};
    for (int i = 0; i < 4; i++) {
        bool on = mask & (1 << i);
        if (ImGui::Checkbox(names[i], &on)) { mask = on ? (mask | (1 << i)) : (mask & ~(1 << i)); changed = true; }
    }
    if (changed) a.engine.nfc().setTechMask(mask);
    float thr = t.powerThreshold;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::SliderFloat("Field threshold", &thr, 0.001f, 0.3f, "%.3f", ImGuiSliderFlags_Logarithmic)) a.engine.nfc().setPowerThreshold(thr);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The magnitude (full scale 1.0) below which there is no reader field. Lower it when a weak field is not detected, raise it when noise is taken for one.");
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Tune to 13.560 MHz and hold the antenna (a loop, or the radio's own antenna input with a short wire) near the reader; the field itself "
                        "is the carrier, the reader's commands are its pauses and the card's answers ride on an 847.5 kHz subcarrier. 10 Msps is what the decoder "
                        "is tuned for; 2.5 Msps is the least that carries both sidebands. Demodulation and decoding: nfc-laboratory (GPL-3.0).");
    ImGui::PopTextWrapPos();
}

void tab(App& a) {
    privacyNote();
    subNav("nfcv", S.view, {"Frames", "Cards", "Field and settings"});
    if (!live(a)) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "tune to 13.56 MHz and start the receiver"); ImGui::PopTextWrapPos(); return; }
    if (S.view == 0) framesView(a); else if (S.view == 1) cardsView(a); else fieldView(a);
}

void list(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::NfcTelemetry& t = a.rx.nfc;
    if (!ImGui::BeginTable("##nfcside", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupColumn("UID"); ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthStretch);
    for (const auto& c : t.cards) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(dect2::nfc::hex(c.uid.data(), c.uid.size()).c_str()); ImGui::PopFont();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.type.empty() ? dect2::nfc::techName(c.tech) : c.type.c_str());
    }
    ImGui::EndTable();
    if (t.cards.empty()) ImGui::TextDisabled(t.fieldOn ? "field present, no card yet" : "no reader field");
}

void receiver(App& a) {
    if (!live(a)) return;
    const dect2::NfcTelemetry& t = a.rx.nfc;
    ImGui::TextUnformatted(dect2::nfcSummary(t).c_str());
    ImGui::Text("level %.1f dBFS, %llu good frames, %llu damaged, field threshold %.3f", t.levelDb, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.powerThreshold);
}

void tick(App&) {}

void status(App& a) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("State");
    ImGui::SameLine();
    ImGui::TextUnformatted(live(a) ? dect2::nfcSummary(a.rx.nfc).c_str() : (a.engine.running() ? "starting" : "stopped"));
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "NFC / RFID sniffer";
    if (live(a)) l2 = dect2::nfcSummary(a.rx.nfc);
}

} // namespace

extern const ModeUi kNfcUi;
const ModeUi kNfcUi = {
    .sideTitle = "CARDS",
    .tabName = "NFC",
    .tabIcon = Ic::Radio,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tick = tick,
};
