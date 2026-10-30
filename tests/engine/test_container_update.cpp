// Updates of the Docker image from inside the app (dect2/engine/container_update.h): the version rule the helper shares, reading the
// helper's status file (also broken and hostile ones), and what the update window shows for each status, request and age.
#include "dect2/engine/container_update.h"
#include <cstdio>
#include <string>

using namespace dect2::cupdate;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static Status st(State s, const std::string& ver, double t, const std::string& msg = "") {
    Status x; x.valid = true; x.state = s; x.version = ver; x.time = t; x.message = msg; return x;
}

int main() {
    // the version rule (packaging/proxmox/saurcom-update checks the same)
    for (const char* ok : {"0.3.1", "v0.3.1", "1", "10.20.30.40", "v2.0"}) CHECK(validVersion(ok), "valid version %s", ok);
    for (const char* bad : {"", "v", "0.3.", ".3", "0..3", "0.3.1-beta.1", "0.3.1 ", " 0.3.1", "1.2.3.4.5", "latest", "-rf", "0.3;reboot",
                            "$(id)", "0.3\n1", "../../etc", "V0.3", "0x10", "123456789012345678901234567890123"})
        CHECK(!validVersion(bad), "invalid version \"%s\"", bad);
    CHECK(validRequest("latest") && validRequest("latest\n") && validRequest("0.3.1\n") && validRequest("v0.3.1\r\n"), "requests");
    CHECK(!validRequest("") && !validRequest("latest; rm -rf /") && !validRequest("0.3.1\nlatest") && !validRequest("LATEST"), "bad requests");
    CHECK(stripV("v0.3.1") == "0.3.1" && stripV("0.3.1") == "0.3.1", "stripV");

    // the status file
    Status s = parseStatus(R"({"state":"pulling","version":"0.3.1","message":"Downloading the new version","time":1761600000})");
    CHECK(s.valid && s.state == State::Pulling && s.version == "0.3.1" && s.message == "Downloading the new version" && s.time == 1761600000, "pulling");
    s = parseStatus(R"({"state":"done","version":"v0.3.1","message":"","time":5})");
    CHECK(s.valid && s.state == State::Done && s.version == "0.3.1", "a v in the version is dropped");
    s = parseStatus(R"({"state":"failed","version":"0.3.1; rm","message":"a\nb\tc\u0007d","time":7})");
    CHECK(s.valid && s.state == State::Failed && s.version.empty() && s.message == "a b c d", "bad version dropped, control characters blanked: %s", s.message.c_str());
    s = parseStatus("{\"state\":\"failed\",\"message\":\"" + std::string(1000, 'x') + "\",\"time\":1}");
    CHECK(s.valid && s.message.size() <= kMaxMessage + 3, "long message cut (%zu)", s.message.size());
    std::string utf;
    for (int i = 0; i < 300; i++) utf += "\xc3\xa9";   // é, two bytes
    s = parseStatus("{\"state\":\"failed\",\"message\":\"" + utf + "\",\"time\":1}");
    CHECK(s.valid && s.message.size() % 2 == 1 && s.message.compare(s.message.size() - 3, 3, "…") == 0, "cut at a character boundary");
    CHECK(s.message.size() - 3 <= kMaxMessage && (s.message.size() - 3) % 2 == 0, "whole characters kept");
    for (const char* bad : {"", "{", "[]", "null", "\"done\"", R"({"state":"exploded","time":1})", R"({"version":"0.3.1"})", R"({"state":1})"})
        CHECK(!parseStatus(bad).valid, "invalid status %s", bad);
    CHECK(!parseStatus(std::string(70000, ' ')).valid, "too long");
    s = parseStatus(R"({"state":"checking"})");
    CHECK(s.valid && s.state == State::Checking && s.time == 0 && s.version.empty(), "missing fields");

    // what the window shows
    const double T = 1761600000;
    Inputs in; in.running = "0.3.0"; in.now = T;
    CHECK(decide(in) == View::Idle, "nothing known");
    in.available = "0.3.1";
    CHECK(decide(in) == View::Available, "a newer release");
    // a request, no answer yet, then no answer at all
    in.requestedAt = T; in.now = T + 5;
    CHECK(decide(in) == View::Waiting, "waiting for the helper");
    in.now = T + kAnswerTimeout;
    CHECK(decide(in) == View::NoAnswer, "no helper answered");
    // an old status (from before the request) is no answer
    in.status = st(State::Done, "0.3.0", T - 3600);
    CHECK(decide(in) == View::NoAnswer, "a status from before the request is no answer");
    in.now = T + 3;
    CHECK(decide(in) == View::Waiting, "...nor while waiting");
    // the helper's steps
    in.status = st(State::Checking, "", T + 1);
    CHECK(decide(in) == View::Checking, "checking");
    in.now = T + 300;
    in.status = st(State::Pulling, "0.3.1", T + 2);
    CHECK(decide(in) == View::Pulling, "pulling, also after the 30 s");
    in.status = st(State::Restarting, "0.3.1", T + 200);
    CHECK(decide(in) == View::Restarting, "restarting");
    in.status = st(State::Done, "0.3.1", T + 290);
    CHECK(decide(in) == View::Restarting, "done, but this is still the old program: restarting");
    in.now = T + 290 + kRestartGrace;
    CHECK(decide(in) == View::Done, "done for another version, long after: done");
    in.status = st(State::Failed, "0.3.1", T + 100, "could not download");
    CHECK(decide(in) == View::Failed, "failed");
    in.status = st(State::Done, "0.3.1", T - 1);
    in.now = T + 1;
    CHECK(decide(in) == View::Restarting, "2 s of slack for whole seconds");

    // after the restart: a new program, no request of its own
    Inputs after; after.running = "0.3.1"; after.now = T + 400;
    after.status = st(State::Done, "0.3.1", T + 350);
    CHECK(decide(after) == View::Done, "updated to 0.3.1");
    after.dismissedAt = T + 350;
    CHECK(decide(after) == View::Idle, "dismissed");
    after.dismissedAt = 0;
    after.now = T + 350 + kResultShown;
    CHECK(decide(after) == View::Idle, "an old result is not shown");
    after.now = T + 400;
    after.status = st(State::Done, "0.3.2", T + 350);
    CHECK(decide(after) == View::Idle, "done for a version that is not running: not shown");
    after.status = st(State::Done, "", T + 350);
    CHECK(decide(after) == View::Idle, "done without a version: not shown");
    // a rollback: the old program again, with the failure
    Inputs rb; rb.running = "0.3.0"; rb.available = "0.3.1"; rb.now = T + 400;
    rb.status = st(State::Failed, "0.3.1", T + 380, "0.3.1 did not start; back to 0.3.0");
    CHECK(decide(rb) == View::Failed, "the failure after a rollback");
    rb.dismissedAt = T + 380;
    CHECK(decide(rb) == View::Available, "dismissed: offered again");
    // an update the program did not ask for (the timer, or a terminal) while it runs
    Inputs bg; bg.running = "0.3.0"; bg.now = T + 10;
    bg.status = st(State::Pulling, "0.3.1", T);
    CHECK(decide(bg) == View::Pulling, "a background update in progress");
    bg.now = T + kBusyStale - 1;
    CHECK(decide(bg) == View::Pulling, "a heartbeat just under the limit: still pulling");
    bg.now = T + kBusyStale;
    CHECK(decide(bg) == View::Stalled, "no heartbeat for 5 minutes: the updater stopped answering");
    bg.now = T + kResultShown;
    CHECK(decide(bg) == View::Idle, "a helper that died long ago");
    bg.now = T + kBusyStale; bg.dismissedAt = T;
    CHECK(decide(bg) == View::Idle, "...dismissed");
    bg.dismissedAt = 0;
    // the same while this program waits for its own request
    Inputs rq; rq.running = "0.3.0"; rq.requestedAt = T; rq.now = T + kBusyStale + 10;
    rq.status = st(State::Restarting, "0.3.1", T + 5);
    CHECK(decide(rq) == View::Stalled, "the program's own request, no heartbeat: stalled");
    rq.status = st(State::Pulling, "0.3.1", T + kBusyStale - 30);
    CHECK(decide(rq) == View::Pulling, "the program's own request, a fresh heartbeat: pulling");
    CHECK(kBusyStale >= 5 * 45 && kBusyStale <= 600, "the limit leaves room for several heartbeats (45 s) and is a few minutes");
    bg.status = Status();
    CHECK(decide(bg) == View::Idle, "no status");

    CHECK(busy(View::Waiting) && busy(View::Checking) && busy(View::Pulling) && busy(View::Restarting), "busy views");
    CHECK(!busy(View::Idle) && !busy(View::Available) && !busy(View::Done) && !busy(View::Failed) && !busy(View::NoAnswer) && !busy(View::Stalled), "idle views");
    CHECK(std::string(stateName(State::Pulling)) == "pulling" && std::string(stateName(State::None)) == "none", "state names");

    // the one-time command: pinned to this release's tag, a temporary file, installed only when complete
    {
        const std::string c = helperCommand("v0.3.1"), c2 = helperCommand("0.3.1");
        CHECK(c == c2, "the v is dropped");
        CHECK(c.find("/Eye_of_SaurCom/v0.3.1/packaging/proxmox/saurcom-update") != std::string::npos, "pinned to the tag: %s", c.c_str());
        CHECK(c.find("/main/") == std::string::npos && c.find("vv") == std::string::npos, "no branch");
        CHECK(c.find("mktemp") != std::string::npos && c.find("[ -s ") != std::string::npos && c.find("#!/usr/bin/env bash") != std::string::npos &&
              c.find("install -m 755") != std::string::npos && c.find("-o /usr/local/bin") == std::string::npos, "a temporary file, checked, then installed");
        CHECK(c.rfind("pct exec <id> -- bash -c '", 0) == 0 && c.back() == '\'' && c.size() < 500, "one quoted command that fits the window's field (%zu)", c.size());
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("container_update: all passed\n");
    return 0;
}
