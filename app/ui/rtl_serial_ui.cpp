#include "rtl_serial_ui.h"
#include "../app.h"
#include "../platform/platform.h"
#include "../state/radio_names.h"
#include "../state/radio_watch.h"
#include "dect2/engine/device_claims.h"
#include "dect2/source/rtl_serial.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <thread>

namespace {

enum class Phase { Idle, Confirm, Working, Done, Failed, Restore, Restoring };

struct State {
    Phase phase = Phase::Idle;
    int rx = 0;                          // the receiver whose toolbar draws the window
    dect2::RadioRow row;                 // the dongle as it was listed when the window was opened
    std::string shown;                   // its name for the window
    std::string dir;                     // <data>/radios
    std::vector<std::string> taken;      // serials of every attached radio
    char serial[16] = {0};
    std::string planned;                 // the backup file this write would make
    std::string backup;                  // the backup that exists
    std::string err, doneNote;
    std::vector<std::string> backups;    // for Restore
    int pick = -1;
    std::string picked;                  // a file from the file dialog
    bool restoreFromFail = false;
    bool opened = false;                 // the popup has been opened for this window
} S;

struct Job {
    dect2::DeviceInfo dev;
    std::string serial, dir, restoreFile, backup, err;
    bool restore = false, ok = false;
} gJob;
std::thread gTh;
std::atomic<bool> gFinished{false};

bool isRtl(const App& a, int idx) {
    const dect2::RadioRow* r = radioRow(a, idx);
    return r && dect2::rtlIsRtl2832(r->dev);
}

std::string backupDir() { return plat::dataDir() + "/radios"; }

void startJob() {
    if (gTh.joinable()) gTh.join();
    gJob.dev = S.row.dev;
    gJob.dir = backupDir();
    gFinished = false;
    radioWatchEepromBusy(true);   // no listing meanwhile (the claim inside the worker also keeps one from starting)
    gTh = std::thread([] {
        if (gJob.restore) gJob.ok = dect2::rtlRestoreEeprom(gJob.dev, gJob.restoreFile, gJob.dir, gJob.backup, gJob.err);
        else gJob.ok = dect2::rtlWriteSerial(gJob.dev, gJob.serial, gJob.dir, gJob.backup, gJob.err);
        gFinished = true;
    });
}

// the worker has finished: take the result on the UI thread
void collect() {
    if (!gFinished.load() || !gTh.joinable()) return;
    gTh.join();
    radioWatchEepromBusy(false);
    S.err = gJob.err;
    if (!gJob.backup.empty()) S.backup = gJob.backup;
    if (gJob.ok) {
        S.phase = Phase::Done;
        if (!gJob.restore) {
            std::string e;
            radionames::carryLabel(S.row, gJob.serial, e);   // its name follows the new serial
            if (!e.empty()) S.doneNote = e;
        }
    } else S.phase = Phase::Failed;
}

void collectTaken(const App& a) {
    S.taken.clear();
    for (const auto& d : a.devices) if (!d.serial.empty()) S.taken.push_back(d.serial);
}

void open(App& a, int idx, bool restore) {
    if (S.phase == Phase::Working || S.phase == Phase::Restoring || !isRtl(a, idx)) return;
    S = State();
    S.rx = a.rxId;
    S.row = *radioRow(a, idx);
    S.shown = radioShownName(a, idx);
    S.dir = backupDir();
    collectTaken(a);
    const uint32_t seed = (uint32_t)time(nullptr) ^ (uint32_t)std::hash<std::string>()(S.row.key);
    snprintf(S.serial, sizeof S.serial, "%s", dect2::rtlPickSerial(S.taken, seed).c_str());
    S.planned = dect2::rtlBackupPath(S.dir, S.row.dev.serial, time(nullptr));
    if (restore) {
        S.backups = dect2::rtlListBackups(S.dir);
        S.pick = S.backups.empty() ? -1 : 0;
        S.phase = Phase::Restore;
    } else S.phase = Phase::Confirm;
}

const ImVec4 kWarn(0.95f, 0.78f, 0.25f, 1), kBad(1.f, 0.45f, 0.4f, 1), kGood(0.45f, 0.9f, 0.5f, 1);

std::string fileName(const std::string& p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

void dongleLines() {
    ImGui::TextDisabled("%s", tr("Dongle:"));
    ImGui::SameLine();
    ImGui::TextUnformatted(S.shown.c_str());   // a name is text, never a format string
    ImGui::TextDisabled("%s", tr("USB port:"));
    ImGui::SameLine();
    ImGui::TextUnformatted(S.row.dev.usbPath.empty() ? tr("unknown") : S.row.dev.usbPath.c_str());
    ImGui::TextDisabled("%s", tr("Serial now:"));
    ImGui::SameLine();
    ImGui::TextUnformatted(S.row.dev.serial.empty() ? tr("(none)") : S.row.dev.serial.c_str());
}

void replugWarning() {
    ImGui::TextColored(kWarn, "%s", tr("The dongle must be unplugged and plugged back in afterwards. Do not unplug it while writing."));
}

}  // namespace

void rtlSerialRequest(App& a, int idx) { open(a, idx, false); }
void rtlSerialRestoreRequest(App& a, int idx) { open(a, idx, true); }

bool rtlSerialBusy() { return S.phase == Phase::Working || S.phase == Phase::Restoring; }

void rtlSerialShutdown() {
    if (gTh.joinable()) gTh.join();
    radioWatchEepromBusy(false);
}

static void inUseTip(const dect2::DeviceInfo& d) {
    const int held = dect2::deviceHolder(d);
    if (held && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(tr("In use by %s: stop it first."), dect2::deviceHolderText(held).c_str());
}

void rtlSerialButtons(App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !isRtl(a, a.devIdx)) return;
    const dect2::DeviceInfo& d = a.devices[(size_t)a.devIdx];
    const bool busy = dect2::deviceHolder(d) != 0 || S.phase == Phase::Working || S.phase == Phase::Restoring;
    ImGui::BeginDisabled(busy);
    if (ImGui::SmallButton(tr("Give this RTL-SDR a unique serial…##rtlser"))) rtlSerialRequest(a, a.devIdx);
    ImGui::EndDisabled();
    inUseTip(d);
    if (!busy && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Writes a new serial number into the dongle's memory, after saving a backup of it. Needed when several dongles share one serial."));
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::SmallButton(tr("Restore from backup…##rtlres"))) rtlSerialRestoreRequest(a, a.devIdx);
    ImGui::EndDisabled();
    inUseTip(d);
}

void rtlSerialWarningButton(App& a, bool small) {
    int idx = -1;
    for (int i = 2; i < (int)a.devices.size(); i++) {
        const dect2::RadioRow* r = radioRow(a, i);
        if (!r || !dect2::rtlIsRtl2832(r->dev) || !r->sharedSerial || r->dupIndex == 0) continue;
        if (idx < 0 || i == a.devIdx) idx = i;
    }
    if (idx < 0) return;
    const bool busy = dect2::deviceHolder(a.devices[(size_t)idx]) != 0 || S.phase == Phase::Working || S.phase == Phase::Restoring;
    ImGui::BeginDisabled(busy);
    const std::string label = std::string(small ? tr("Fix…") : tr("Give this RTL-SDR a unique serial…")) + "##rtlserw";
    if (small ? ImGui::SmallButton(label.c_str()) : ImGui::Button(label.c_str())) rtlSerialRequest(a, idx);
    ImGui::EndDisabled();
    inUseTip(a.devices[(size_t)idx]);
    if (!busy && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Give the RTL-SDR that shares a serial a unique one, so the radios can be told apart."));
}

void rtlSerialModal(App& a) {
    if (S.phase == Phase::Idle || S.rx != a.rxId) return;
    collect();
    const char* const kId = "RTL-SDR serial###rtlserial";
    const bool working = S.phase == Phase::Working || S.phase == Phase::Restoring;
    if (!S.opened || (working && !ImGui::IsPopupOpen(kId))) { ImGui::OpenPopup(kId); S.opened = true; }   // a running write keeps its window (Esc cannot close it)
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kId, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (S.phase != Phase::Working && S.phase != Phase::Restoring) S.phase = Phase::Idle;
        return;
    }
    ImGui::PushTextWrapPos(460 * gUi);
    bool close = false;
    switch (S.phase) {
    case Phase::Confirm: {
        ImGui::TextUnformatted(tr("Give this RTL-SDR a unique serial"));
        ImGui::Separator();
        dongleLines();
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("New serial:"));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150 * gUi);
        ImGui::InputText("##rtlnewser", S.serial, sizeof S.serial, ImGuiInputTextFlags_CharsDecimal);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Pick another"))) {
            static uint32_t bump = 1;
            snprintf(S.serial, sizeof S.serial, "%s", dect2::rtlPickSerial(S.taken, (uint32_t)time(nullptr) * 7919u + bump++).c_str());
        }
        std::string verr;
        const bool usable = dect2::rtlSerialUsable(S.serial, S.taken, verr);
        if (!usable) ImGui::TextColored(kBad, "%s", verr.c_str());
        else ImGui::TextDisabled("%s", tr("8 digits, different from every other attached radio."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Backup of the old contents:"));
        ImGui::TextUnformatted(S.planned.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", tr("This changes the permanent memory (EEPROM) of the dongle. The old contents are saved first, and the result is read back to check it."));
        replugWarning();
        ImGui::Spacing();
        ImGui::BeginDisabled(!usable);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.2f, 0.15f, 1));
        if (ImGui::Button(tr("Write the new serial"))) {
            gJob.restore = false;
            gJob.serial = S.serial;
            gJob.backup = S.planned; gJob.err.clear(); gJob.ok = false;   // the file shown is the file made
            S.phase = Phase::Working;
            startJob();
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) close = true;
        break;
    }
    case Phase::Working:
    case Phase::Restoring: {
        ImGui::TextUnformatted(S.phase == Phase::Working ? tr("Writing the new serial…") : tr("Restoring the backup…"));
        ImGui::TextDisabled("%s", tr("This takes a few seconds, and up to a few minutes if the dongle does not answer."));
        const double t = ImGui::GetTime();
        ImGui::ProgressBar((float)(t - (long)t), ImVec2(300 * gUi, 0), "");
        ImGui::TextColored(kWarn, "%s", tr("Do not unplug the dongle while it is writing."));
        break;
    }
    case Phase::Done:
        if (gJob.restore) ImGui::TextColored(kGood, "%s", tr("Done. The backup is back on the dongle. Unplug the dongle and plug it back in."));
        else ImGui::TextColored(kGood, "%s", tr("Done. Unplug the dongle and plug it back in to use its new serial."));
        ImGui::Spacing();
        if (!gJob.restore) {
            ImGui::TextDisabled("%s", tr("New serial:"));
            ImGui::SameLine();
            ImGui::TextUnformatted(gJob.serial.c_str());
        }
        if (!S.backup.empty()) {
            ImGui::TextDisabled("%s", gJob.restore ? tr("What the dongle held before is saved at:") : tr("The old contents are saved at:"));
            ImGui::TextUnformatted(S.backup.c_str());
        }
        if (!S.doneNote.empty()) ImGui::TextColored(kWarn, "%s", S.doneNote.c_str());
        ImGui::Spacing();
        if (ImGui::Button(tr("Close"))) close = true;
        break;
    case Phase::Failed:
        ImGui::TextColored(kBad, "%s", gJob.restore ? tr("The restore did not finish.") : tr("The serial was not written."));
        ImGui::TextUnformatted(S.err.c_str());   // text from the library: never a format string
        ImGui::Spacing();
        if (!S.backup.empty() && !gJob.restore && ImGui::Button(tr("Restore from backup…"))) {
            const std::string keep = S.backup;
            S.backups = dect2::rtlListBackups(S.dir);
            S.picked = keep;
            S.pick = -1;
            S.err.clear();
            S.phase = Phase::Restore;
            S.restoreFromFail = true;
            break;
        }
        if (!S.backup.empty() && !gJob.restore) ImGui::SameLine();
        if (ImGui::Button(tr("Close"))) close = true;
        break;
    case Phase::Restore: {
        ImGui::TextUnformatted(tr("Restore an EEPROM backup"));
        ImGui::Separator();
        dongleLines();
        ImGui::Spacing();
        if (S.backups.empty() && S.picked.empty()) ImGui::TextDisabled("%s", tr("No backups in the radios folder. Choose a backup file."));
        for (int i = 0; i < (int)S.backups.size() && i < 12; i++) {
            const std::string nm = fileName(S.backups[(size_t)i]) + "##bk" + std::to_string(i);
            if (ImGui::Selectable(nm.c_str(), S.pick == i && S.picked.empty())) { S.pick = i; S.picked.clear(); }
        }
        if (ImGui::SmallButton(tr("Choose a file…"))) {
            const std::string f = openFileDialog();
            if (!f.empty()) { S.picked = f; S.pick = -1; }
        }
        const std::string file = !S.picked.empty() ? S.picked : (S.pick >= 0 && S.pick < (int)S.backups.size() ? S.backups[(size_t)S.pick] : std::string());
        if (!file.empty()) { ImGui::TextDisabled("%s", tr("Backup to put back:")); ImGui::TextUnformatted(file.c_str()); }
        ImGui::Spacing();
        ImGui::TextWrapped("%s", tr("This overwrites the dongle's EEPROM with the contents of the backup file, then reads it back to check it."));
        replugWarning();
        ImGui::Spacing();
        ImGui::BeginDisabled(file.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.2f, 0.15f, 1));
        if (ImGui::Button(tr("Restore this backup"))) {
            gJob.restore = true;
            gJob.restoreFile = file;
            gJob.backup.clear(); gJob.err.clear(); gJob.ok = false;
            S.phase = Phase::Restoring;
            startJob();
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) close = true;
        break;
    }
    default: break;
    }
    ImGui::PopTextWrapPos();
    if (close) { S.phase = Phase::Idle; ImGui::CloseCurrentPopup(); }
    ImGui::EndPopup();
}
