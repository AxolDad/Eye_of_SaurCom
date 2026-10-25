// The live context store: see context.h
#include "dect2/transcribe/context.h"
#include "dect2/transcribe/transcriber.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2 {
namespace {

// Case-insensitive compare, so the same name arriving from two feeders counts once.
std::string foldCase(const std::string& s) {
    std::string o = s;
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return o;
}

// How much a term is worth now: its weight, decayed by how long ago it was seen. Half an hour halves it, so a name heard in the last minute
// comfortably outranks one from an hour ago even if the old one was given a bigger weight.
double score(const ContextTerm& t, int64_t nowMs) {
    const double ageSec = t.seenMs > 0 ? std::max(0.0, (double)(nowMs - t.seenMs) / 1000.0) : 36000.0;
    return (double)t.weight * std::exp(-ageSec / 2600.0);
}

}  // namespace

std::string scopeForFreq(double freqHz) {
    char b[48];
    snprintf(b, sizeof b, "freq:%lld", (long long)llround(freqHz));
    return b;
}

std::string scopeForChannel(const std::string& name) { return "chan:" + name; }

std::vector<std::string> scopesFor(const SourceInfo& src) {
    std::vector<std::string> v;
    v.push_back("all");
    if (!src.domain.empty()) v.push_back(src.domain);
    if (!src.channel.empty()) v.push_back(scopeForChannel(src.channel));
    if (src.freqHz > 0) v.push_back(scopeForFreq(src.freqHz));
    return v;
}

ContextStore& ContextStore::global() {
    static ContextStore s;
    return s;
}

void ContextStore::setTerms(const std::string& scope, std::vector<ContextTerm> terms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (terms.empty()) scopes_.erase(scope);
    else scopes_[scope] = std::move(terms);
}

void ContextStore::addTerm(const std::string& scope, const ContextTerm& term) {
    if (term.spoken.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ContextTerm>& v = scopes_[scope];
    const std::string key = foldCase(term.spoken);
    for (ContextTerm& t : v)
        if (foldCase(t.spoken) == key) { t = term; return; }
    v.push_back(term);
}

void ContextStore::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    scopes_.clear();
}

void ContextStore::clearScope(const std::string& scope) {
    std::lock_guard<std::mutex> lk(mu_);
    scopes_.erase(scope);
}

size_t ContextStore::scopeCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return scopes_.size();
}

size_t ContextStore::termCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t n = 0;
    for (const auto& kv : scopes_) n += kv.second.size();
    return n;
}

std::vector<std::string> ContextStore::scopes() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> v;
    v.reserve(scopes_.size());
    for (const auto& kv : scopes_) v.push_back(kv.first);
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<ContextTerm> ContextStore::termsFor(const SourceInfo& src, size_t maxChars, int64_t nowMs, int64_t maxAgeMs) const {
    const std::vector<std::string> want = scopesFor(src);
    std::vector<ContextTerm> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        // Narrower scopes are collected last and win the de-duplication, so a frequency's own name for something beats the domain's.
        for (const std::string& sc : want) {
            const auto it = scopes_.find(sc);
            if (it == scopes_.end()) continue;
            for (const ContextTerm& t : it->second) {
                if (t.spoken.empty()) continue;
                if (maxAgeMs > 0 && t.seenMs > 0 && nowMs - t.seenMs > maxAgeMs) continue;
                const std::string key = foldCase(t.spoken);
                auto dup = std::find_if(all.begin(), all.end(), [&](const ContextTerm& o) { return foldCase(o.spoken) == key; });
                if (dup != all.end()) *dup = t;
                else all.push_back(t);
            }
        }
    }
    std::stable_sort(all.begin(), all.end(), [&](const ContextTerm& a, const ContextTerm& b) { return score(a, nowMs) > score(b, nowMs); });
    // Trim to the budget by joined length, counting the ", " between terms as the caller will write it.
    std::vector<ContextTerm> out;
    size_t used = 0;
    for (const ContextTerm& t : all) {
        const size_t add = t.spoken.size() + (out.empty() ? 0 : 2);
        if (used + add > maxChars) continue;   // skip and keep going: a long name should not hide every shorter one behind it
        out.push_back(t);
        used += add;
    }
    return out;
}

std::string ContextStore::promptFor(const SourceInfo& src, size_t maxChars, int64_t nowMs, int64_t maxAgeMs) const {
    // "Expect: " is part of the budget the caller gave, so take it off the top rather than overrunning the prompt.
    static const std::string kLead = "Expect: ";
    if (maxChars <= kLead.size() + 1) return "";
    const std::vector<ContextTerm> v = termsFor(src, maxChars - kLead.size() - 1, nowMs, maxAgeMs);
    if (v.empty()) return "";
    std::string s = kLead;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) s += ", ";
        s += v[i].spoken;
    }
    s += ".";
    return s;
}

}  // namespace dect2
