// Updates in the window: a check when the program starts (every start, one retry on failure), the download in the background, a button in the top bar that tells
// the state, and a small window with the notes of the new version and the settings. The new version is put in place when the program closes,
// or at once on request.
// In the Docker image (packaging/docker) the program cannot replace its own image: there the button asks the helper outside the container
// (saurcom-update, packaging/proxmox) through a request file in the config volume and shows the status file it writes back into a
// directory of root's that this container sees read-only (dect2/engine/container_update.h). The desktop download and install-on-close are not offered there.
#include "../app.h"
#include "dect2/engine/updater.h"
#include "dect2/engine/container_update.h"
#include "../state/receivers.h"
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace {
Updater& U(App& a) {
    if (!a.upd) { a.upd.reset(new Updater(SAURCOM_VERSION)); a.upd->loadResult(); }   // loadResult: what the installation of an earlier start left behind
    return *a.upd;
}
bool resultDismissed = false;
// the version whose installation failed at the last start: not tried again by itself (the button still does)
bool failedBefore(const Updater::Status& st, const ReleaseInfo& r) { return st.lastResult.found && !st.lastResult.ok && compareVersions(r.version, st.lastResult.version) == 0; }
double nowS() { return (double)time(nullptr); }
bool skipped(const App& a, const ReleaseInfo& r) { return !a.updSkip.empty() && compareVersions(r.version, a.updSkip) <= 0; }

// ---- the Docker image: the request and status files
namespace cu = dect2::cupdate;
// the one-time step for a Proxmox container made before the update button: the saurcom-update of this release, which then also installs its
// units (pinned to this version's tag)
const std::string& helperCmd() { static const std::string c = cu::helperCommand(SAURCOM_VERSION); return c; }
struct Cu {
    bool on = false;          // this program runs in a container: updates go through the helper outside it
    std::string dir;          // where the request and auto files are (/config/.saurcom-update, the app's)
    std::string statusDir;    // where the helper's status and log are (/run/saurcom-update, root's, read-only here)
    bool dirOk = false;       // the directory exists (or was made) and this program may write in it
    double requestedAt = 0;   // when this run wrote a request (seconds since 1970); 0: none
    std::string requested;    // the version it asked for
    double dismissedAt = 0;   // results written at or before this were dismissed
    cu::Status st;            // the status file as last read
    double nextPoll = 0;      // ImGui time of the next read
    bool autoOn = false;      // the auto file says 1
    std::string err;          // why the last request or auto file could not be written
    cu::View last = cu::View::Idle;
};
std::string readSmall(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::string s(65537, '\0');
    f.read(&s[0], (std::streamsize)s.size());
    s.resize((size_t)f.gcount());
    return s;
}
bool dirUsable(const std::string& dir) {
#ifdef _WIN32
    (void)dir; return false;
#else
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return std::filesystem::is_directory(dir, ec) && access(dir.c_str(), W_OK) == 0;
#endif
}
// a whole file in one step: a temporary file next to it, then a rename (the helper never reads half a request)
bool putFile(const std::string& dir, const std::string& name, const std::string& text, std::string& err) {
#ifdef _WIN32
    (void)dir; (void)name; (void)text; err = "not on Windows"; return false;
#else
    if (!dirUsable(dir)) { err = "cannot write to " + dir; return false; }
    const std::string tmp = dir + "/." + name + ".tmp" + std::to_string((long)getpid()), dst = dir + "/" + name;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot write " + tmp; return false; }
        f << text;
        f.close();
        if (!f) { err = "cannot write " + tmp; return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec);
    if (ec) { std::error_code e2; std::filesystem::remove(tmp, e2); err = "cannot write " + dst + ": " + ec.message(); return false; }
    return true;
#endif
}
void cuPoll(Cu& c) {
    c.st = cu::parseStatus(readSmall(c.statusDir + "/status"));
    std::string au = readSmall(c.dir + "/auto");
    c.autoOn = !au.empty() && au[0] == '1';
}
Cu& C() {
    static Cu c;
    static bool init = false;
    if (!init) {
        init = true;
#ifndef _WIN32
        // SAURCOM_CONTAINER: set by packaging/docker/Dockerfile ("0" switches this off); /.dockerenv (Docker) and /run/.containerenv (Podman):
        // made by the container runtime in every container
        const char* e = getenv("SAURCOM_CONTAINER");
        if (e && *e) c.on = strcmp(e, "0") != 0;
        else c.on = access("/.dockerenv", F_OK) == 0 || access("/run/.containerenv", F_OK) == 0;
        const char* d = getenv("SAURCOM_UPDATE_DIR");   // for tests and screenshots
        c.dir = d && *d ? d : cu::kDefaultDir;
        const char* sd = getenv("SAURCOM_UPDATE_STATUS_DIR");
        c.statusDir = sd && *sd ? sd : cu::kStatusDir;
        if (c.on) { c.dirOk = dirUsable(c.dir); cuPoll(c); }
#endif
    }
    return c;
}
cu::View cuView(const App& a, const Updater::Status& st) {
    Cu& c = C();
    cu::Inputs in;
    in.running = cu::stripV(SAURCOM_VERSION);
    if (st.phase == Updater::Phase::Available && !skipped(a, st.release) && cu::validVersion(st.release.version)) in.available = cu::stripV(st.release.version);
    in.requestedAt = c.requestedAt;
    in.dismissedAt = c.dismissedAt;
    in.status = c.st;
    in.now = nowS();
    return cu::decide(in);
}
bool cuRequest(App& a, const std::string& version) {
    Cu& c = C();
    c.err.clear();
    if (!cu::validVersion(version)) { c.err = "not a version: " + version; return false; }
    if (!putFile(c.dir, "request", cu::stripV(version) + "\n", c.err)) { c.dirOk = false; a.engine.log("update: " + c.err); return false; }
    c.dirOk = true;
    c.requestedAt = nowS();
    c.requested = cu::stripV(version);
    c.nextPoll = 0;
    a.engine.log("update: asked the updater outside this container for Eye of SaurCom " + c.requested);
    return true;
}
void cuDismiss(Cu& c) {
    if (c.requestedAt > 0 && c.st.time < c.requestedAt - 2) { std::error_code ec; std::filesystem::remove(c.dir + "/request", ec); }   // unanswered: not left for later
    c.requestedAt = 0;
    c.dismissedAt = c.st.valid ? c.st.time : nowS();
    c.err.clear();
}
// polls the status file: once a second while an update is under way, else once a minute; logs what changed
void cuTick(App& a, const Updater::Status& st) {
    Cu& c = C();
    if (!c.on) return;
    const double t = ImGui::GetTime();
    cu::View v = cuView(a, st);
    if (t >= c.nextPoll) {
        cuPoll(c);
        v = cuView(a, st);
        c.nextPoll = t + (cu::busy(v) ? 1.0 : 60.0);
    }
    if (v == c.last) return;
    c.last = v;
    switch (v) {
    case cu::View::Pulling: a.engine.log("update: downloading Eye of SaurCom " + c.st.version + " (outside this container; it keeps running meanwhile)"); break;
    case cu::View::Restarting: a.engine.log("update: restarting with Eye of SaurCom " + c.st.version); break;
    case cu::View::Done: a.engine.log("updated to v" + c.st.version); break;
    case cu::View::Failed: a.engine.log("update: " + (c.st.message.empty() ? std::string("failed") : c.st.message)); break;
    case cu::View::NoAnswer: {
        std::error_code ec; std::filesystem::remove(c.dir + "/request", ec);   // no helper: the request is not left lying for one installed later
        a.engine.log("update: no updater answered in 30 s (see the update window)");
        break;
    }
    default: break;
    }
}
}

void updateTick(App& a) {
    Updater& u = U(a);
    const Updater::Status st = u.status();
    static Updater::Phase lastPhase = Updater::Phase::Idle;
    static double startT = -1;
    static bool resultLogged = false;
    if (!resultLogged) {
        resultLogged = true;
        if (st.lastResult.found) {
            if (st.lastResult.ok) a.engine.log("updated to v" + st.lastResult.version);
            else a.engine.log("the update to v" + st.lastResult.version + " did not install (setup exit code " + std::to_string(st.lastResult.setupExit) + ")");
        }
    }
    if (startT < 0) startT = ImGui::GetTime();
    // a check a few seconds after every start (in the background), and one retry if it fails, e.g. the network was not up yet
    static int startAttempts = 0;
    static double failT = -1;
    if (startAttempts > 0 && startAttempts < 2 && st.phase == Updater::Phase::Failed && failT < 0) failT = ImGui::GetTime();
    const bool container = C().on;
    if (st.phase == Updater::Phase::Idle || st.phase == Updater::Phase::Failed) {
        const int act = startupCheckAction(a.updCheck && !getenv("SAURCOM_NO_UPDATE_CHECK"), ImGui::GetTime() - startT, startAttempts,
                                           failT >= 0 && st.phase == Updater::Phase::Failed, failT >= 0 ? ImGui::GetTime() - failT : 0);
        if (act) { startAttempts++; a.updStarted = true; u.check(a.updPre && !container); }   // the image has no beta tags
    }
    cuTick(a, st);
    if (st.phase != lastPhase) {
        lastPhase = st.phase;
        switch (st.phase) {
        case Updater::Phase::UpToDate: a.updLast = nowS(); savePrefs(a); a.engine.log(std::string("Eye of SaurCom ") + SAURCOM_VERSION + " is the newest version"); break;
        case Updater::Phase::Available:
            a.updLast = nowS(); savePrefs(a);
            a.engine.log("update available: Eye of SaurCom " + st.release.version + (st.release.prerelease ? tr(" (beta)") : ""));
            if (!container && a.updAuto && st.autoInstallable && !skipped(a, st.release) && !failedBefore(st, st.release)) { U(a).download(); a.engine.log("downloading Eye of SaurCom " + st.release.version + " in the background"); }
            break;
        case Updater::Phase::Ready: a.engine.log("Eye of SaurCom " + st.release.version + " is ready: it is installed when you close the program"); break;
        case Updater::Phase::Failed: a.engine.log("update: " + st.error); break;
        default: break;
        }
    }
}

// Called when the program is closing: puts the downloaded version in place. Returns true if a helper was started.
bool updateOnExit(App& a) {
    if (!a.upd || C().on) return false;   // in a container the helper outside it updates the image
    const Updater::Status st = a.upd->status();
    if (st.phase != Updater::Phase::Ready || !a.updAuto) return false;
    std::string err;
    return a.upd->apply(false, &err);
}

namespace {
const ImVec4 kOk(0.35f, 0.9f, 0.45f, 1), kBad(0.95f, 0.5f, 0.35f, 1), kBusy(0.4f, 0.75f, 0.95f, 1), kNew(0.95f, 0.75f, 0.2f, 1);

// what to do when the button cannot ask a helper: the one-time step for an older Proxmox container, and plain Docker
void cuInstructions(const Cu& c, bool notWritable) {
    ImGui::PushTextWrapPos(0);
    if (notWritable) ImGui::TextColored(kBad, tr("Eye of SaurCom cannot write to %s, so it cannot ask for the update."), c.dir.c_str());
    else ImGui::TextColored(kBad, "%s", tr("No updater answered. A Proxmox container set up before this button needs a newer saurcom-update once."));
    ImGui::TextUnformatted(tr("On the Proxmox node, in its Shell, run this once (your container's number in place of <id>):"));
    ImGui::PopTextWrapPos();
    static char cmd[512];
    snprintf(cmd, sizeof cmd, "%s", helperCmd().c_str());
    ImGui::SetNextItemWidth(-70 * gUi);
    ImGui::InputText("##helpercmd", cmd, sizeof cmd, ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button(tr("Copy##helpercmd"))) ImGui::SetClipboardText(helperCmd().c_str());
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(tr("It updates Eye of SaurCom now, and from then on the Update now button works. pct exec <id> -- saurcom-update also still works."));
    ImGui::TextDisabled("%s", tr("Docker without Proxmox: run docker compose pull, then docker compose up -d, next to compose.yaml."));
    ImGui::PopTextWrapPos();
}

void cuWindow(App& a, const Updater::Status& st) {
    Cu& c = C();
    const cu::View v = cuView(a, st);
    const char* reconnect = tr("The page will reconnect by itself in about half a minute.");
    auto updateNow = [&](const char* label, const std::string& ver) {
        if (!c.dirOk && !dirUsable(c.dir)) { cuInstructions(c, true); return; }
        c.dirOk = true;
        ImGui::TextDisabled("%s", reconnect);
        if (ImGui::Button(label)) cuRequest(a, ver);
    };
    ImGui::PushTextWrapPos(0);
    switch (v) {
    case cu::View::Idle:
        switch (st.phase) {
        case Updater::Phase::Checking: ImGui::TextUnformatted(tr("Asking GitHub for the newest version…")); break;
        case Updater::Phase::UpToDate: ImGui::TextColored(kOk, "%s", tr("You have the newest version.")); break;
        case Updater::Phase::Failed: ImGui::TextColored(kBad, "%s", st.error.c_str()); break;
        case Updater::Phase::Available: ImGui::Text(tr("You skipped version %s."), st.release.version.c_str()); break;
        default: ImGui::TextUnformatted(tr("Not checked yet.")); break;
        }
        break;
    case cu::View::Available:
        ImGui::Text(tr("Eye of SaurCom %s is available."), st.release.version.c_str());
        if (!st.release.notes.empty()) {
            ImGui::BeginChild("##notes", ImVec2(0, 150 * gUi), true);
            ImGui::TextUnformatted(st.release.notes.c_str());
            ImGui::EndChild();
        }
        updateNow(tr("Update now"), st.release.version);
        ImGui::SameLine();   // (no "Release page": the container has no browser of its own to open it in)
        if (ImGui::Button(tr("Skip this version"))) { a.updSkip = st.release.version; savePrefs(a); }
        break;
    case cu::View::Waiting:
        ImGui::Text(tr("Asking for Eye of SaurCom %s…"), c.requested.c_str());
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), tr("waiting for the updater"));
        break;
    case cu::View::Checking:
        ImGui::TextUnformatted(tr("Looking for the new version…"));
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "");
        break;
    case cu::View::Pulling:
        ImGui::TextUnformatted(tr("Downloading the new version…"));
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "");
        if (!c.st.message.empty()) ImGui::TextDisabled("%s", c.st.message.c_str());
        ImGui::TextDisabled("%s", tr("Eye of SaurCom keeps running until the download is complete."));
        break;
    case cu::View::Restarting:
        ImGui::TextUnformatted(tr("Restarting…"));
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 0), "");
        ImGui::TextDisabled("%s", reconnect);
        break;
    case cu::View::Done:
        ImGui::TextColored(kOk, tr("Updated to %s."), c.st.version.c_str());
        if (!c.st.version.empty() && c.st.version != cu::stripV(SAURCOM_VERSION)) ImGui::TextDisabled("%s", tr("Reload the page if it still shows the old version."));
        if (ImGui::Button(tr("Dismiss##cudone"))) cuDismiss(c);
        break;
    case cu::View::Failed: {
        ImGui::TextColored(kBad, "%s", c.st.message.empty() ? tr("The update did not complete.") : c.st.message.c_str());
        ImGui::TextDisabled(tr("Log: %s/log (in the Proxmox container: /opt/saurcom/update/log)"), c.statusDir.c_str());
        if (st.phase == Updater::Phase::Available && cu::validVersion(st.release.version)) {
            updateNow(tr("Try again"), st.release.version);
            ImGui::SameLine();
        }
        if (ImGui::Button(tr("Dismiss##cufailed"))) cuDismiss(c);
        break;
    }
    case cu::View::NoAnswer:
        cuInstructions(c, false);
        if (ImGui::Button(tr("Back##cunoanswer"))) cuDismiss(c);
        break;
    }
    ImGui::PopTextWrapPos();
}
}

void updateButton(App& a) {
    Updater& u = U(a);
    const Updater::Status st = u.status();
    char label[64];
    ImVec4 col(0.62f, 0.68f, 0.75f, 1);
    const bool container = C().on;
    const cu::View cv = container ? cuView(a, st) : cu::View::Idle;
    if (container && cv != cu::View::Idle) {
        switch (cv) {
        case cu::View::Available: snprintf(label, sizeof label, tr("update %s"), st.release.version.c_str()); col = kNew; break;
        case cu::View::Waiting: case cu::View::Checking: snprintf(label, sizeof label, "%s", tr("updating…")); col = kBusy; break;
        case cu::View::Pulling: snprintf(label, sizeof label, "%s", tr("downloading…")); col = kBusy; break;
        case cu::View::Restarting: snprintf(label, sizeof label, "%s", tr("restarting…")); col = kBusy; break;
        case cu::View::Done:   // in the top bar for 10 minutes after the update; the window shows it until dismissed (or for a day)
            if (nowS() - C().st.time < 600) { snprintf(label, sizeof label, tr("updated to %s"), C().st.version.c_str()); col = kOk; }
            else snprintf(label, sizeof label, "v%s", SAURCOM_VERSION);
            break;
        default: snprintf(label, sizeof label, "v%s", SAURCOM_VERSION); col = kBad; break;   // failed, no answer
        }
    } else switch (st.phase) {
    case Updater::Phase::Checking: snprintf(label, sizeof label, "%s", tr("checking…")); break;
    case Updater::Phase::Available: snprintf(label, sizeof label, tr("update %s"), st.release.version.c_str()); col = ImVec4(0.95f, 0.75f, 0.2f, 1); if (skipped(a, st.release)) { snprintf(label, sizeof label, "v%s", SAURCOM_VERSION); col = ImVec4(0.62f, 0.68f, 0.75f, 1); } break;
    case Updater::Phase::Downloading: snprintf(label, sizeof label, tr("downloading %.0f%%"), st.total ? 100.0 * (double)st.done / (double)st.total : 0.0); col = ImVec4(0.4f, 0.75f, 0.95f, 1); break;
    case Updater::Phase::Preparing: snprintf(label, sizeof label, "%s", tr("preparing…")); col = ImVec4(0.4f, 0.75f, 0.95f, 1); break;
    case Updater::Phase::Ready: snprintf(label, sizeof label, "%s", tr("restart to update")); col = ImVec4(0.35f, 0.9f, 0.45f, 1); break;
    case Updater::Phase::Failed: snprintf(label, sizeof label, "v%s", SAURCOM_VERSION); col = ImVec4(0.95f, 0.5f, 0.35f, 1); break;
    default: snprintf(label, sizeof label, "v%s", SAURCOM_VERSION); break;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (ImGui::SmallButton(label)) { ImGui::OpenPopup("##updates"); if (container) C().nextPoll = 0; }
    ImGui::PopStyleColor();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Updates"));
    { static const bool dbg = getenv("SAURCOM_OPEN_UPDATES") != nullptr; if (dbg && st.phase != Updater::Phase::Idle && st.phase != Updater::Phase::Checking && !ImGui::IsPopupOpen("##updates")) ImGui::OpenPopup("##updates"); }   // for screenshots
    ImGui::SetNextWindowPos(ImVec2(bmax.x + 70 * gUi, bmax.y + 4), ImGuiCond_Always, ImVec2(1, 0));   // under the button, its right edge near the window's
    ImGui::SetNextWindowSizeConstraints(ImVec2(400 * gUi, 0), ImVec2(520 * gUi, 520 * gUi));
    if (ImGui::BeginPopup("##updates")) {
        ImGui::Text(tr("Eye of SaurCom %s"), SAURCOM_VERSION);
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("(%s)", container ? tr("Docker image") : installKindName(st.kind));
        ImGui::Separator();
        if (container) {
            cuWindow(a, st);
            const bool busy = st.phase == Updater::Phase::Checking || cu::busy(cv);
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(tr("Check now"))) { a.updSkip.clear(); u.check(false); }
            ImGui::EndDisabled();
            ImGui::Separator();
            Cu& c = C();
            if (ImGui::Checkbox(tr("Check for updates automatically"), &a.updCheck)) savePrefs(a);
            bool au = c.autoOn;
            if (ImGui::Checkbox(tr("Install updates automatically (at 04:00)"), &au)) {
                if (putFile(c.dir, "auto", au ? "1\n" : "0\n", c.err)) { c.autoOn = au; c.err.clear(); a.engine.log(au ? "update: automatic updates on (daily at 04:00)" : "update: automatic updates off"); }
                else a.engine.log("update: " + c.err);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every day at 04:00 (plus up to half an hour), the updater outside this container installs a newer release by itself. The page drops for about half a minute then."));
            if (!c.err.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(kBad, "%s", c.err.c_str()); ImGui::PopTextWrapPos(); }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", tr("Only github.com is contacted. The new image is downloaded and started outside this container, by saurcom-update; if it does not start, the previous one is started again."));
            ImGui::PopStyleColor();
            ImGui::EndPopup();
            return;
        }
        if (st.lastResult.found && !st.lastResult.ok && !resultDismissed) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), tr("The update to v%s did not install (setup exit code %d)."), st.lastResult.version.c_str(), st.lastResult.setupExit);
            ImGui::PopTextWrapPos();
            if (ImGui::Button(tr("Download page"))) u.openReleasePage();
            ImGui::SameLine();
            if (ImGui::Button(tr("Dismiss"))) resultDismissed = true;
            ImGui::Separator();
        }
        switch (st.phase) {
        case Updater::Phase::Checking: ImGui::TextUnformatted(tr("Asking GitHub for the newest version…")); break;
        case Updater::Phase::UpToDate: ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "%s", tr("You have the newest version.")); break;
        case Updater::Phase::Failed: ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "%s", st.error.c_str()); break;
        case Updater::Phase::Available:
        case Updater::Phase::Downloading:
        case Updater::Phase::Preparing:
        case Updater::Phase::Ready: {
            ImGui::Text(tr("Version %s is available%s."), st.release.version.c_str(), st.release.prerelease ? tr(" (beta)") : "");
            if (!st.release.notes.empty()) {
                ImGui::BeginChild("##notes", ImVec2(0, 150 * gUi), true);
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(st.release.notes.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }
            if (st.phase == Updater::Phase::Downloading) ImGui::ProgressBar(st.total ? (float)((double)st.done / (double)st.total) : 0.f, ImVec2(-1, 0));
            else if (st.phase == Updater::Phase::Preparing) ImGui::TextUnformatted(tr("Checking and preparing the new version…"));
            else if (st.phase == Updater::Phase::Ready) {
                ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "%s", tr("Downloaded and checked. It is installed when you close Eye of SaurCom."));
                if (ImGui::Button(tr("Restart now and update"))) {
                    std::string err;
                    if (u.apply(true, &err)) glfwSetWindowShouldClose(gWindow, 1); else a.engine.log(err.compare(0, 6, "Update") == 0 ? err : "update: " + err);
                }
                if (!st.applyError.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(ImVec4(0.95f, 0.5f, 0.35f, 1), "%s", st.applyError.c_str()); ImGui::PopTextWrapPos(); }
                if (st.kind == InstallKind::WindowsInstall) ImGui::TextDisabled("%s", tr("Windows asks for permission to run the installer."));
                if (st.kind == InstallKind::LinuxDeb) ImGui::TextDisabled("%s", tr("The system asks for your password to install the package."));
            } else {
                if (st.autoInstallable) {
                    if (ImGui::Button(tr("Download and install"))) u.download();
                }
                else { ImGui::TextDisabled("%s", tr("This copy of Eye of SaurCom cannot replace itself.")); }
                ImGui::SameLine();
                if (ImGui::Button(tr("Release page"))) u.openReleasePage();
                ImGui::SameLine();
                if (ImGui::Button(tr("Skip this version"))) { a.updSkip = st.release.version; savePrefs(a); }
            }
            break;
        }
        default: ImGui::TextUnformatted(tr("Not checked yet.")); break;
        }
        const bool busy = st.phase == Updater::Phase::Checking || st.phase == Updater::Phase::Downloading || st.phase == Updater::Phase::Preparing;
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(tr("Check now"))) { a.updSkip.clear(); u.check(a.updPre); }
        ImGui::EndDisabled();
        ImGui::Separator();
        bool ch = false;
        ch |= ImGui::Checkbox(tr("Check for updates automatically"), &a.updCheck);
        ch |= ImGui::Checkbox(tr("Download and install them by themselves"), &a.updAuto);
        ch |= ImGui::Checkbox(tr("Include beta versions"), &a.updPre);
        if (ch) savePrefs(a);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", tr("Only github.com is contacted. Downloads are checked against the SHA-256 GitHub lists for the file."));
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }
}

// "Clear all" next to the version: empties the decoded lists of every mode, on every open receiver, after a question
void clearAllButton(App& a) {
    ImGui::SameLine(0, 6 * gUi);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.6f, 0.45f, 1));
    if (ImGui::SmallButton(tr("Clear all##clearall"))) ImGui::OpenPopup("##clearallask");
    ImGui::PopStyleColor();
    const ImVec2 bmax = ImGui::GetItemRectMax();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Clears all previous data: the decoded lists of every mode (aircraft, ships, messages, pictures and so on), on every open receiver. The saved copy on disk is emptied too."));
    ImGui::SetNextWindowPos(ImVec2(bmax.x, bmax.y + 4), ImGuiCond_Appearing, ImVec2(1, 0));
    if (ImGui::BeginPopup("##clearallask")) {
        ImGui::PushTextWrapPos(340 * gUi);
        ImGui::TextUnformatted(tr("Are you sure? This clears all previous data and cannot be undone."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Yes, clear all"))) {
            a.engine.clearAllResults();
            marineFaxClearUi();
            for (App* r : receiversAll()) if (r != &a) r->engine.clearAllResults();
            a.engine.log("results: all previous data cleared");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
