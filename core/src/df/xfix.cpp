// Cross-site bearing fixes (core/include/dect2/df/xfix.h).
#include "dect2/df/xfix.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace dect2::df {

void CrossFixer::add(const automation::Event& ev) {
    auto num = [&](const char* k, double& v) {
        auto it = ev.f.find(k);
        if (it == ev.f.end() || it->second.t != automation::Value::Num || !std::isfinite(it->second.n)) return false;
        v = it->second.n;
        return true;
    };
    Entry n{};
    if (!num("bearing_deg", n.bearing) || !num("lat", n.lat) || !num("lon", n.lon)) return;
    if (!num("sigma_deg", n.sigma)) n.sigma = 5;
    // remote input is untrusted
    if (n.sigma <= 0 || n.lat < -90 || n.lat > 90 || n.lon < -180 || n.lon > 180) return;
    if (!std::isfinite(ev.freqHz) || ev.freqHz <= 0 || ev.freqHz > 1e12 || !std::isfinite(ev.time)) return;
    if (std::fabs(n.bearing) > 720) return;
    n.bearing = std::fmod(std::fmod(n.bearing, 360.0) + 360.0, 360.0);
    auto s = ev.f.find("site");
    if (s != ev.f.end()) n.site = s->second.text();
    n.time = ev.time; n.freqHz = ev.freqHz;

    std::lock_guard<std::mutex> lk(mu_);
    e_.erase(std::remove_if(e_.begin(), e_.end(), [&](const Entry& o) { return o.time < n.time - kMaxAgeS; }), e_.end());
    // the newest bearing per site and (about) frequency replaces the older one
    for (Entry& o : e_)
        if (o.site == n.site && std::fabs(o.freqHz - n.freqHz) <= kFreqTolHz / 2) { if (n.time >= o.time) o = n; return; }
    e_.push_back(n);
    if (e_.size() > 2000) e_.erase(e_.begin(), e_.begin() + 1000);   // never grows without bound if fixes() is not called
}

std::vector<CrossFix> CrossFixer::fixes(double now) const {
    std::vector<Entry> v;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const Entry& x : e_) if (now - x.time <= kMaxAgeS) v.push_back(x);
    }
    std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.freqHz < b.freqHz; });
    std::vector<CrossFix> out;
    std::vector<char> used(v.size(), 0);
    for (size_t i = 0; i < v.size(); i++) {
        if (used[i]) continue;
        // the group: every unused bearing within the tolerances of the seed (the lowest frequency), one per site
        std::vector<size_t> g{i};
        for (size_t j = i + 1; j < v.size() && v[j].freqHz - v[i].freqHz <= kFreqTolHz; j++) {
            if (used[j] || std::fabs(v[j].time - v[i].time) > kTimeTolS) continue;
            bool dup = false;
            for (size_t k : g) dup = dup || v[k].site == v[j].site;
            if (!dup) g.push_back(j);
        }
        if (g.size() < 2) continue;
        std::vector<BearingLine> lines;
        for (size_t k : g) lines.push_back({v[k].lat, v[k].lon, v[k].bearing, v[k].sigma});
        Fix f = fixPositionRobust(lines);
        if (!f.ok) continue;
        CrossFix c;
        c.fix = f;
        std::set<std::string> sites;
        double fsum = 0;
        for (size_t n = 0; n < g.size(); n++) {
            const Entry& x = v[g[n]];
            used[g[n]] = 1;
            fsum += x.freqHz;
            c.time = std::max(c.time, x.time);
            if (n < f.inlier.size() && f.inlier[n]) sites.insert(x.site);
        }
        if (sites.size() < 2) continue;   // the lines that agreed came from fewer than two sites
        c.freqHz = fsum / (double)g.size();
        c.sites.assign(sites.begin(), sites.end());
        out.push_back(std::move(c));
    }
    return out;
}

}  // namespace dect2::df
