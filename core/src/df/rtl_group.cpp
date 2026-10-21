#include "dect2/df/rtl_group.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace dect2::df {

static bool hasWord(std::string s, const char* w) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s.find(w) != std::string::npos;
}

std::vector<RtlInfo> findHub(const std::vector<RtlInfo>& all) {
    std::vector<RtlInfo> h;
    for (auto& d : all) {
        long s = d.serial.empty() ? -1 : std::strtol(d.serial.c_str(), nullptr, 10);
        bool numeric = !d.serial.empty() && std::all_of(d.serial.begin(), d.serial.end(), [](char c) { return std::isdigit((unsigned char)c); });
        if ((numeric && s >= 1000 && s <= 1004) || hasWord(d.product, "kraken") || hasWord(d.product, "kerberos")) h.push_back(d);
    }
    if (h.size() < 2) h.clear();
    std::sort(h.begin(), h.end(), [](const RtlInfo& a, const RtlInfo& b) { return a.serial < b.serial; });
    return h;
}

std::string hubLabel(const std::vector<RtlInfo>& h) {
    bool ker = !h.empty() && hasWord(h[0].product, "kerberos");
    return std::string(ker ? "KerberosSDR (" : "KrakenSDR (") + std::to_string(h.size()) + " ch)";
}

namespace {
struct RtlFeed : ChannelFeed {
    RtlLink* l;
    explicit RtlFeed(RtlLink* link) : l(link) {}
    size_t read(cd* out, size_t n) override {
        std::vector<uint8_t> b(n * 2);
        size_t have = 0;
        while (have < b.size()) {
            int g = l->readSync(b.data() + have, (int)(b.size() - have));
            if (g <= 0) break;
            have += (size_t)g;
        }
        size_t k = have / 2;
        for (size_t i = 0; i < k; i++) out[i] = cd((b[2 * i] - 127.5) / 127.5, (b[2 * i + 1] - 127.5) / 127.5);
        return k;
    }
};
}

std::unique_ptr<ChannelFeed> makeRtlFeed(RtlLink* link) { return std::make_unique<RtlFeed>(link); }

bool startHub(std::vector<std::unique_ptr<RtlLink>> links, const HubStart& s, Hub& out) {
    if (links.size() < 2) return false;
    for (auto& l : links) if (!l || !l->configure(s.freqHz, s.rateHz, s.gainTenthsDb)) return false;
    std::vector<std::unique_ptr<ChannelFeed>> feeds;
    for (auto& l : links) feeds.push_back(makeRtlFeed(l.get()));
    out.links = std::move(links);
    out.noise = std::make_unique<GpioNoiseSwitch>(out.links[0].get());
    out.group = std::make_unique<CoherentGroup>(std::move(feeds), s.rateHz, s.freqHz);
    return true;
}

bool calibrateHub(Hub& h, NoiseSwitch& sw, size_t n, int maxLag) {
    if (!h.group || !sw.set(true)) return false;
    bool ok = h.group->calibrate(n, maxLag);
    sw.set(false);
    return ok;
}

}   // namespace dect2::df
