// Updates of the Docker image (packaging/docker), started from inside the app. The app cannot replace its own image, so it asks a helper
// outside the container through small files, with no network port and no Docker socket. The app writes only into its own config volume,
// and the helper (root) only reads there; the helper writes only into a directory of root's that the app sees read-only:
//   request   (the app's, kDefaultDir) the version it wants ("0.3.1", or "latest"), through a temporary file and a rename; the helper
//             reads it only if it is a plain file of the app's, and removes it
//   auto      (the app's, kDefaultDir) 1 or 0: the helper's daily timer installs a newer release by itself only with 1
//   status    (root's, kStatusDir) the helper (saurcom-update, packaging/proxmox) writes {"state":"checking|pulling|restarting|done|failed",
//             "version":"0.3.1","message":"...","time":<seconds since 1970>} at every step
//   log       (root's, kStatusDir) the helper's log, capped in size
// What is here is the part without files or a window: the version rule (the same as the helper's), reading the status, and what the
// update window shows for a given status (decide()).
#pragma once
#include "dect2/automation/json_mini.h"
#include <string>

namespace dect2::cupdate {

inline constexpr const char* kDefaultDir = "/config/.saurcom-update";   // in the container; /opt/saurcom/config/.saurcom-update in the Proxmox container
inline constexpr const char* kStatusDir = "/run/saurcom-update";       // read-only in the container; /opt/saurcom/update (root's) in the Proxmox container
inline constexpr double kAnswerTimeout = 30;        // seconds the helper has to write a status after a request
inline constexpr double kBusyStale = 3 * 3600;      // a "pulling" or "restarting" this old is a helper that died: not shown
inline constexpr double kResultShown = 24 * 3600;   // a "done" or "failed" is shown this long after it was written (unless dismissed)
inline constexpr double kRestartGrace = 120;        // "done" for another version while this one still runs: shown as restarting this long

// A release version as the helper accepts it: digits and dots (one to four numbers), optionally with a leading "v"; at most 32 characters.
// No letters, so no beta tags and nothing the helper could read as an option or a path.
inline bool validVersion(const std::string& v) {
    if (v.empty() || v.size() > 32) return false;
    size_t i = v[0] == 'v' ? 1 : 0;
    if (i >= v.size()) return false;
    bool digit = false;
    int dots = 0;
    for (; i < v.size(); i++) {
        const char c = v[i];
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && digit && dots < 3) { digit = false; dots++; }
        else return false;
    }
    return digit;
}
inline std::string stripV(const std::string& v) { return !v.empty() && v[0] == 'v' ? v.substr(1) : v; }
// What the request file may hold (one line): "latest" or a valid version.
inline bool validRequest(const std::string& s) {
    std::string t = s;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    return t == "latest" || validVersion(t);
}

// The one-time step for a Proxmox container made before the update button, run on the Proxmox node: the saurcom-update of this release
// (the tag v<version>, never a branch), downloaded to a temporary file, installed only when it is complete (not empty, the script's first
// line), then run. <id> stays for the user to fill in.
inline std::string helperCommand(const std::string& version) {
    return "pct exec <id> -- bash -c 'f=$(mktemp) && curl -fsSL https://raw.githubusercontent.com/AxolDad/Eye_of_SaurCom/v" + stripV(version) +
           "/packaging/proxmox/saurcom-update -o \"$f\" && [ -s \"$f\" ] && head -n 1 \"$f\" | grep -qx \"#!/usr/bin/env bash\""
           " && install -m 755 \"$f\" /usr/local/bin/saurcom-update; r=$?; rm -f \"$f\"; [ $r = 0 ] && saurcom-update'";
}

enum class State { None, Checking, Pulling, Restarting, Done, Failed };
inline const char* stateName(State s) {
    switch (s) {
    case State::Checking: return "checking";
    case State::Pulling: return "pulling";
    case State::Restarting: return "restarting";
    case State::Done: return "done";
    case State::Failed: return "failed";
    default: return "none";
    }
}

struct Status {
    bool valid = false;        // a status file with a known state was read
    State state = State::None;
    std::string version;       // without the "v"; empty when the file held none or an invalid one
    std::string message;       // the helper's words: control characters removed, at most kMaxMessage bytes (drawn with "%s")
    double time = 0;           // seconds since 1970 when the helper wrote it
};
inline constexpr size_t kMaxMessage = 400;

// Reads the status file's text. Anything that is not a JSON object with a known "state" gives valid = false.
inline Status parseStatus(const std::string& text) {
    Status st;
    if (text.empty() || text.size() > 65536) return st;
    automation::json::Value v;
    std::string err;
    if (!automation::json::parse(text, v, err) || v.type != automation::json::Value::Obj) return st;
    const std::string s = v.str("state");
    if (s == "checking") st.state = State::Checking;
    else if (s == "pulling") st.state = State::Pulling;
    else if (s == "restarting") st.state = State::Restarting;
    else if (s == "done") st.state = State::Done;
    else if (s == "failed") st.state = State::Failed;
    else return st;
    const std::string ver = v.str("version");
    if (validVersion(ver)) st.version = stripV(ver);
    std::string m;
    for (unsigned char c : v.str("message")) {
        if (c < 0x20 || c == 0x7f) c = ' ';
        m += (char)c;
    }
    if (m.size() > kMaxMessage) {
        size_t cut = kMaxMessage;
        while (cut > 0 && ((unsigned char)m[cut] & 0xC0) == 0x80) cut--;   // not inside a UTF-8 sequence
        m = m.substr(0, cut) + "…";
    }
    st.message = m;
    st.time = v.num("time");
    st.valid = true;
    return st;
}

// What the update window shows.
enum class View {
    Idle,        // nothing to do (or nothing known): the usual version button
    Available,   // a newer release: "Update now"
    Waiting,     // a request was written, no answer yet
    Checking,    // the helper is looking for the release
    Pulling,     // downloading the new image (the running version is untouched)
    Restarting,  // recreating the container: the page drops for a while
    Done,        // "Updated to X"
    Failed,      // the helper's message, and where its log is
    NoAnswer     // no helper answered within kAnswerTimeout: the plain instructions
};
inline bool busy(View v) { return v == View::Waiting || v == View::Checking || v == View::Pulling || v == View::Restarting; }

struct Inputs {
    std::string running;       // this program's version
    std::string available;     // a newer release the check found; empty when none or not known
    double requestedAt = 0;    // when this run of the app wrote a request (seconds since 1970); 0: none
    double dismissedAt = 0;    // a status written at or before this was dismissed by the user
    Status status;             // the status file now
    double now = 0;            // seconds since 1970
};

inline View fromStatus(const Status& s, const std::string& running, double now) {
    switch (s.state) {
    case State::Checking: return View::Checking;
    case State::Pulling: return View::Pulling;
    case State::Restarting: return View::Restarting;
    case State::Failed: return View::Failed;
    case State::Done:
        // the helper is done but this is still the old program: the container is being recreated (the page drops in a moment)
        if (!s.version.empty() && !running.empty() && s.version != running && now - s.time < kRestartGrace) return View::Restarting;
        return View::Done;
    default: return View::Idle;
    }
}

inline View decide(const Inputs& in) {
    const Status& s = in.status;
    if (in.requestedAt > 0) {
        // only a status written after the request answers it (2 s of slack: the helper's clock is the same, its seconds are whole)
        if (s.valid && s.time >= in.requestedAt - 2) return fromStatus(s, in.running, in.now);
        return in.now - in.requestedAt >= kAnswerTimeout ? View::NoAnswer : View::Waiting;
    }
    if (s.valid && s.time > in.dismissedAt) {
        const double age = in.now - s.time;
        switch (s.state) {
        case State::Checking: case State::Pulling: case State::Restarting:
            if (age < kBusyStale) return fromStatus(s, in.running, in.now);   // e.g. the daily timer, or saurcom-update run in a terminal
            break;
        case State::Done:
            // after the restart: "Updated to X" when X is what runs now
            if (age < kResultShown && !s.version.empty() && s.version == in.running) return View::Done;
            break;
        case State::Failed:
            if (age < kResultShown) return View::Failed;
            break;
        default: break;
        }
    }
    return in.available.empty() ? View::Idle : View::Available;
}

}  // namespace dect2::cupdate
