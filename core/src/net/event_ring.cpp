#include "dect2/net/event_ring.h"
#include "dect2/automation/json_mini.h"

namespace dect2 {

namespace aj = automation::json;

std::string eventToJson(const automation::Event& e) {
    aj::Value v = aj::Value::object();
    v.set("type", aj::Value::string(e.type));
    v.set("time", aj::Value::number(e.time));
    v.set("mode", aj::Value::string(e.mode));
    v.set("freq", aj::Value::number(e.freqHz));
    aj::Value f = aj::Value::object();
    for (const auto& kv : e.f) {
        switch (kv.second.t) {
        case automation::Value::Num: f.set(kv.first, aj::Value::number(kv.second.n)); break;
        case automation::Value::Str: f.set(kv.first, aj::Value::string(kv.second.s)); break;
        case automation::Value::Bool: f.set(kv.first, aj::Value::boolean(kv.second.b)); break;
        default: f.set(kv.first, aj::Value()); break;
        }
    }
    v.set("f", std::move(f));
    return aj::dump(v);
}

bool eventFromJson(const std::string& line, automation::Event& out, std::string& err) {
    aj::Value v;
    if (!aj::parse(line, v, err)) return false;
    if (v.type != aj::Value::Obj) { err = "event is not an object"; return false; }
    const aj::Value* t = v.get("type");
    if (!t || t->type != aj::Value::Str || t->s.empty()) { err = "missing type"; return false; }
    automation::Event e(t->s);
    e.time = v.num("time");
    e.mode = v.str("mode");
    e.freqHz = v.num("freq");
    if (const aj::Value* f = v.get("f")) {
        if (f->type != aj::Value::Obj) { err = "f is not an object"; return false; }
        for (const auto& kv : f->o) {
            const aj::Value& x = kv.second;
            switch (x.type) {
            case aj::Value::Num: e.num(kv.first, x.n); break;
            case aj::Value::Str: e.str(kv.first, x.s); break;   // str() cuts at kMaxFieldText
            case aj::Value::Bool: e.flag(kv.first, x.b); break;
            case aj::Value::Null: e.f[kv.first] = automation::Value(); break;
            default: err = "nested value in f." + kv.first; return false;
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
