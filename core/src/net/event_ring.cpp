#include "dect2/net/event_ring.h"
#include "dect2/automation/json_mini.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace aj = automation::json;

// NaN and Inf have no JSON form (json::dump would write nan/inf and the line would not parse): send null instead.
static aj::Value finiteOrNull(double d) { return std::isfinite(d) ? aj::Value::number(d) : aj::Value(); }

std::string eventToJson(const automation::Event& e) {
    aj::Value v = aj::Value::object();
    v.set("type", aj::Value::string(e.type));
    v.set("time", finiteOrNull(e.time));
    v.set("mode", aj::Value::string(e.mode));
    v.set("freq", finiteOrNull(e.freqHz));
    aj::Value f = aj::Value::object();
    for (const auto& kv : e.f) {
        switch (kv.second.t) {
        case automation::Value::Num: f.set(kv.first, finiteOrNull(kv.second.n)); break;
        case automation::Value::Str: f.set(kv.first, aj::Value::string(kv.second.s)); break;
        case automation::Value::Bool: f.set(kv.first, aj::Value::boolean(kv.second.b)); break;
        default: f.set(kv.first, aj::Value()); break;
        }
    }
    v.set("f", std::move(f));
    return aj::dump(v);
}

// at most n bytes, cut back to the start of a UTF-8 character so that no half character is left at the end
static std::string cutText(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t k = n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    return s.substr(0, k);
}

bool eventFromJson(const std::string& line, automation::Event& out, std::string& err) {
    aj::Value v;
    if (!aj::parse(line, v, err)) return false;
    if (v.type != aj::Value::Obj) { err = "event is not an object"; return false; }
    const aj::Value* t = v.get("type");
    if (!t || t->type != aj::Value::Str || t->s.empty()) { err = "missing type"; return false; }
    automation::Event e(cutText(t->s, kMaxEventName));
    // the line comes from another station: untrusted. A time that cannot be a time is refused; one too far from this clock (a station whose clock
    // is badly wrong) becomes 0, "unknown", which the consumers read as "now", so it cannot push the fixer or the timeline days ahead or back.
    const double tm = v.num("time");
    if (!std::isfinite(tm)) { err = "time is not a finite number"; return false; }
    e.time = std::fabs(tm - automation::wallNow()) > kMaxEventTimeOffSec ? 0 : tm;
    e.mode = cutText(v.str("mode"), kMaxEventName);
    const double fr = v.num("freq");
    e.freqHz = std::isfinite(fr) ? std::min(std::max(fr, 0.0), kMaxEventFreqHz) : 0;
    if (const aj::Value* f = v.get("f")) {
        if (f->type != aj::Value::Obj) { err = "f is not an object"; return false; }
        for (const auto& kv : f->o) {
            const aj::Value& x = kv.second;
            if (x.type == aj::Value::Obj || x.type == aj::Value::Arr) { err = "nested value in f." + cutText(kv.first, kMaxEventName); return false; }
            const std::string key = cutText(kv.first, kMaxEventName);
            if (!e.f.count(key) && e.f.size() >= kMaxEventFields) continue;   // the rest of the fields are dropped
            switch (x.type) {
            case aj::Value::Num: e.num(key, x.n); break;
            case aj::Value::Str: e.str(key, x.s); break;   // str() cuts at kMaxFieldText
            case aj::Value::Bool: e.flag(key, x.b); break;
            case aj::Value::Null: e.f[key] = automation::Value(); break;
            default: err = "bad value in f." + key; return false;
            }
        }
    }
    out = std::move(e);
    return true;
}

EventRing::EventRing(size_t cap) : slots_(cap ? cap : 1) {}

void EventRing::push(const automation::Event& e) {
    if (e.f.count("site")) return;
    std::string line = eventToJson(e);   // serialised outside the lock
    std::lock_guard<std::mutex> lk(mu_);
    slots_[head_ % slots_.size()] = std::move(line);
    head_++;
}

size_t EventRing::since(uint64_t& cursor, std::vector<std::string>& lines, size_t max, uint64_t& missed) const {
    std::lock_guard<std::mutex> lk(mu_);
    missed = 0;
    const uint64_t cap = slots_.size();
    const uint64_t oldest = head_ > cap ? head_ - cap : 0;
    if (cursor > head_) cursor = head_;
    if (cursor < oldest) { missed = oldest - cursor; cursor = oldest; }
    size_t n = 0;
    while (cursor < head_ && n < max) {
        lines.push_back(slots_[cursor % cap]);
        cursor++;
        n++;
    }
    return n;
}

uint64_t EventRing::head() const {
    std::lock_guard<std::mutex> lk(mu_);
    return head_;
}

EventRing& globalEventRing() {
    static EventRing r;
    return r;
}

}  // namespace dect2
