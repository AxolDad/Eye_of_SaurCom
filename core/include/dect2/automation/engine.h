// The automation engine: takes events from the decoders on a bounded queue and its own thread, runs the rules and scripts, calls the action sink.
// A decoder never waits for it: emit() only pushes (and throws the event away when the queue is full).
#pragma once
#include "dect2/automation/catalog.h"
#include "dect2/automation/event.h"
#include "dect2/automation/rules.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace dect2::automation {

// What happens outside the engine (notifications, recording, tuning ...): the application implements it, the tests use a fake.
class ActionSink {
public:
    virtual ~ActionSink() = default;
    // kind: notify, sound, record_iq, record_audio, tune, memory, webhook (p: url, body), screenshot, run (p: program, args one per line).
    // "log" is done by the engine. p holds the values already filled in. Must not block for long (start a thread for a web request).
    virtual bool perform(const std::string& kind, const std::map<std::string, std::string>& p, std::string& detail) = 0;
};

struct Firing {
    double time = 0;
    std::string rule;           // rule or script name
    std::string eventType;
    std::string summary;        // what the event was, a few words
    bool ok = true;
    bool test = false;
    std::vector<std::string> actions;   // "notify: title ...", "webhook: ok" ...
    std::string error;
};

struct FireReport {
    bool matched = false;
    std::string why;                    // why it did not match, or an error
    std::vector<std::string> actions;   // what the actions are (dry run) or did (execute)
};

struct RuleStats { int matched = 0, fired = 0, suppressed = 0, errors = 0; double lastFired = 0; std::string lastError; };

enum Family { FAdsb, FAcars, FVdl2, FHfdl, FPager, FAis, FAprs, FSonde, FGnss, FLora, FBle, FRid, FDsc, FSignal, FReceiver, FSatellite, FTime, FPlugin, kFamilies };   // FPlugin: "plugin.<type>" events that plugins emit (core/include/dect2/plugin)
int familyOf(const std::string& type);   // -1 for an unknown type

class Engine {
public:
    Engine();
    ~Engine();
    void setSink(ActionSink* s);
    void setStation(bool set, double lat, double lon);
    void setLogDir(const std::string& dir);              // where the "log" action writes its CSV files
    void setAllowRun(bool on);                           // the global switch for "run" actions (off by default)
    void setConfirmedRuns(const std::set<std::string>& fingerprints);   // runFingerprint() of the rules the user confirmed
    void setClock(std::function<double()> now);          // tests
    void setTzOffset(int sec);
    void setMaxActionsPerMinute(int n);                  // all rules and scripts together (default 60)

    void setRules(const std::vector<Rule>& r);
    std::vector<Rule> rules() const;
    void setScripts(const std::vector<Script>& s);
    std::vector<Script> scripts() const;
    RuleStats stats(const std::string& id) const;        // of a rule or a script
    std::string scriptError(const std::string& id) const;

    // true when some enabled rule or script wants events of this family (a lock-free read for the decoders)
    bool wants(int family) const { return family >= 0 && family < kFamilies && ((mask_.load(std::memory_order_relaxed) | tapMask_.load(std::memory_order_relaxed) | observeMask_.load(std::memory_order_relaxed) | watchMask_.load(std::memory_order_relaxed) |
                                                                     eyeMask_.load(std::memory_order_relaxed) | listenMask_.load(std::memory_order_relaxed)) >> family & 1); }
    // A tap sees every submitted event of the given families (bit = 1 << Family), on the decoder's thread, whether or not a rule wants it. It must not block
    // (the message database only queues). Pass an empty function to remove it.
    void setTap(std::function<void(const Event&)> f, uint32_t familyMask);
    // An observer sees every submitted event of the given families like a tap does, and can be set beside the tap (the plugin host uses it for action
    // plugins). Same rules as a tap: on the decoder's thread, must not block. An empty function removes it.
    void setObserver(std::function<void(const Event&)> f, uint32_t familyMask);
    // A watcher is a third such hook (the watchlists use it); same rules: on the decoder's thread, must not block.
    void setWatcher(std::function<void(const Event&)> f, uint32_t familyMask);
    // A fourth such hook, for the Eye overview (core/include/dect2/eye/eye.h); same rules: on the decoder's thread, must not block.
    void setOverview(std::function<void(const Event&)> f, uint32_t familyMask);
    // Listeners fan out: any number can be added beside the four fixed hooks (the event exporter uses one). Each sees every submitted event of its
    // families (bit = 1 << Family); same rules as a tap: on the decoder's thread, must not block, never called with a lock held. Returns an id > 0.
    int addListener(std::function<void(const Event&)> f, uint32_t familyMask);
    void removeListener(int id);
    void submit(Event&& ev);                             // queue; never blocks
    uint64_t dropped() const { return dropped_.load(); }

    void start();                                        // the thread
    void stop();
    // The work of the thread, callable from tests: runs one event through all rules and scripts / does the timed jobs for time `now`.
    void process(const Event& ev);
    void tick(double now);
    void drain();                                        // processes what is queued, on the caller's thread

    // Test button: does `ev` match, and what would the actions do. execute: also do them (still no "run" without confirmation); the firing is logged as a test.
    FireReport test(const Rule& r, const Event& ev, bool execute);

    std::vector<Firing> firings() const;                 // oldest first, the last 500
    void clearFirings();
    void setFiringHook(std::function<void(const Firing&)> f);

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
    std::atomic<uint32_t> mask_{0};
    std::atomic<uint32_t> tapMask_{0};
    std::atomic<uint32_t> observeMask_{0};
    std::atomic<uint32_t> watchMask_{0};
    std::atomic<uint32_t> eyeMask_{0};
    struct Listener { int id; uint32_t mask; std::function<void(const Event&)> fn; };
    std::atomic<uint32_t> listenMask_{0};
    std::mutex listenMu_;
    std::vector<Listener> listeners_;
    int nextListenerId_ = 1;
    std::mutex eyeMu_;
    std::function<void(const Event&)> eye_;
    std::mutex watchMu_;
    std::function<void(const Event&)> watch_;
    std::mutex obsMu_;
    std::function<void(const Event&)> obs_;
    std::mutex tapMu_;
    std::function<void(const Event&)> tap_;
    std::atomic<uint64_t> dropped_{0};
};

// ---- the program-wide instance and what the decoders call ----
Engine& global();
inline bool wants(Family f) { return global().wants(f); }
inline void emit(Event&& ev) { global().submit(std::move(ev)); }
// receiver.state: state is started, stopped, dropped, reconnected or overload
inline void receiverState(const char* state, const std::string& detail) {
    if (!wants(FReceiver)) return;
    Event e("receiver.state");
    e.at(wallNow(), "", 0);
    e.str("state", state).str("detail", detail);
    emit(std::move(e));
}

}  // namespace dect2::automation
