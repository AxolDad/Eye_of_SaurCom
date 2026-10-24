// Wi-Fi network monitor: the test signal through the receiver (wifi_gen.h, wifi_rx.h): two APs on channel 6, a bad FCS, carrier
// offsets of +-50 kHz, 12 dB SNR, a sample clock error, another sample rate, and noise alone.
#include "dect2/wifi/wifi_gen.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wifi/wifi_rx.h"
#include "dect2/wifi/wifi_tel.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::wifi;
#include "../common/test_util.h"

static WifiTelemetry run(WifiGenConfig c, double secs, double centerHz = 0) {
    WifiGenerator g(c);
    WifiReceiver r;
    r.configure(c.rate);
    r.setCenterHz(centerHz);
    std::vector<cf32> buf(65536);
    const size_t total = (size_t)(secs * c.rate);
    for (size_t done = 0; done < total; done += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    WifiTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static const WifiAp* find(const WifiTelemetry& t, const char* bssid) {
    for (const auto& a : t.aps) if (a.bssid == bssid) return &a;
    return nullptr;
}

static bool twoAps(const WifiTelemetry& t, const char* what) {
    const WifiAp* h = find(t, "00:04:0E:12:34:56");
    const WifiAp* c = find(t, "FC:EC:DA:AB:CD:EF");
    bool ok = true;
    CHECK(h && h->ssid == "OnAir-Home" && h->security == Wpa2 && h->vendor == "AVM" && h->channel == 6 && h->interval == 100, "%s: home AP", what); ok &= h != nullptr;
    CHECK(c && c->ssid == "OnAir-Cafe" && c->security == Wpa2Wpa3 && c->vendor == "Ubiquiti" && c->channel == 6, "%s: cafe AP", what); ok &= c != nullptr;
    return ok;
}

int main() {
    const auto t0 = std::chrono::steady_clock::now();
    {   // two APs, a hidden one, a probe response and a data frame on channel 6, clean
        WifiGenConfig c;
        c.snrDb = 30;
        c.frames = wifiTestSchedule();
        const WifiTelemetry t = run(c, 0.25, channelMhz(6) * 1e6);
        printf("  clean: %zu APs, %llu ok, %llu bad, %llu headers, SNR %.1f, offset %.0f Hz\n", t.aps.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
               (unsigned long long)t.headersOk, t.snrDb, t.cfoHz);
        twoAps(t, "clean");
        const WifiAp* h = find(t, "00:04:0E:12:34:56");
        CHECK(h && h->beacons >= 12 && h->probeResps >= 12 && h->heardOn == 6 && h->rates == "1 2 5.5 11 6 9 12 18 24 36 48 54" && h->ciphers == "CCMP", "home details");
        const WifiAp* x = find(t, "02:11:22:33:44:55");
        CHECK(x && x->hidden && x->ssid.empty() && x->security == Open && x->interval == 200 && x->vendor == "local" && x->rates == "1 2 5.5 11", "hidden AP");
        CHECK(t.aps.size() == 3 && t.blocksBad == 0 && t.headersBad == 0, "%zu APs, %llu bad", t.aps.size(), (unsigned long long)t.blocksBad);
        CHECK(t.perType[2] >= 12 && t.perType[0] == t.beacons + t.probeResps && t.channel == 6, "data frames counted %llu", (unsigned long long)t.perType[2]);
        CHECK(t.snrDb > 20, "SNR %.1f", t.snrDb);
    }
    {   // deauthentication and disassociation frames are counted and listed (for the flood alarm), and are not taken for access points
        auto mgmt = [](uint8_t fc0, const uint8_t dst[6], const uint8_t src[6], const uint8_t bss[6]) {
            WifiGenFrame g;
            g.mpdu = {fc0, 0x00, 0x00, 0x00};
            g.mpdu.insert(g.mpdu.end(), dst, dst + 6); g.mpdu.insert(g.mpdu.end(), src, src + 6); g.mpdu.insert(g.mpdu.end(), bss, bss + 6);
            g.mpdu.insert(g.mpdu.end(), {0x10, 0x00, 0x07, 0x00});   // sequence control, reason 7
            appendFcs(g.mpdu);
            return g;
        };
        const uint8_t all[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, ap1[6] = {0x00, 0x04, 0x0E, 0x12, 0x34, 0x56}, sta[6] = {0x3C, 0x22, 0xFB, 0x01, 0x02, 0x03};
        WifiGenConfig c;
        c.snrDb = 30;
        c.frames = wifiTestSchedule();
        for (int i = 0; i < 4; i++) c.frames.push_back(mgmt(0xC0, all, ap1, ap1));    // deauthentication to everyone
        c.frames.push_back(mgmt(0xA0, sta, ap1, ap1));                                   // disassociation of one station
        const WifiTelemetry t = run(c, 0.25, channelMhz(6) * 1e6);
        printf("  deauth: %llu deauth, %llu disassoc, %zu listed, %zu APs\n", (unsigned long long)t.deauths, (unsigned long long)t.disassocs, t.deauthLog.size(), t.aps.size());
        CHECK(t.deauths >= 12 && t.disassocs >= 3, "counts: %llu / %llu", (unsigned long long)t.deauths, (unsigned long long)t.disassocs);
        CHECK(!t.deauthLog.empty() && t.deauthLog.size() <= kWifiDeauthMax, "list %zu", t.deauthLog.size());
        bool sawBroadcast = false, sawStation = false;
        for (const auto& d : t.deauthLog) {
            if (!d.disassoc && d.source == "00:04:0E:12:34:56" && d.dest == "FF:FF:FF:FF:FF:FF" && d.bssid == "00:04:0E:12:34:56" && d.channel == 6) sawBroadcast = true;
            if (d.disassoc && d.dest == "3C:22:FB:01:02:03") sawStation = true;
        }
        CHECK(sawBroadcast && sawStation, "addresses: broadcast %d station %d", sawBroadcast, sawStation);
        CHECK(t.aps.size() == 3 && t.perType[0] >= t.beacons + t.probeResps + t.deauths + t.disassocs, "still three APs, management frames counted");
    }
    {   // a bad FCS is rejected, the good frames around it still decode
        WifiGenConfig c;
        c.frames = wifiTestSchedule();
        c.frames[1].badFcs = true;   // the cafe AP's beacon
        const WifiTelemetry t = run(c, 0.26);
        printf("  bad FCS: %zu APs, %llu ok, %llu bad\n", t.aps.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(!find(t, "FC:EC:DA:AB:CD:EF") && find(t, "00:04:0E:12:34:56") && t.blocksBad >= 8, "bad FCS: %llu rejected", (unsigned long long)t.blocksBad);
    }
    for (double cfo : {50e3, -50e3}) {   // carrier offset (some 20 ppm at 2.4 GHz) with 12 dB SNR
        WifiGenConfig c;
        c.snrDb = 12;
        c.cfoHz = cfo;
        c.frames = wifiTestSchedule();
        c.seed = 7;
        const WifiTelemetry t = run(c, 0.25);
        printf("  %+.0f kHz, 12 dB: %zu APs, %llu ok, %llu bad, SNR %.1f, offset %.1f kHz\n", cfo / 1e3, t.aps.size(), (unsigned long long)t.blocksOk,
               (unsigned long long)t.blocksBad, t.snrDb, t.cfoHz / 1e3);
        char w[32]; snprintf(w, sizeof w, "%+.0f kHz", cfo / 1e3);
        twoAps(t, w);
        CHECK(t.blocksOk >= 60 && t.blocksBad <= 2, "%s: %llu ok, %llu bad", w, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(std::fabs(t.cfoHz - cfo) < 15e3, "%s: offset %.0f", w, t.cfoHz);
    }
    {   // a sample clock 25 ppm off and long frames: the timing tracker follows
        WifiGenConfig c;
        c.clockPpm = 25;
        c.snrDb = 20;
        c.frames = wifiTestSchedule();
        const uint8_t sta[6] = {0x3A, 0, 0, 0, 0, 9};
        WifiGenFrame big; big.mpdu = makeDataFrame(c.frames[0].mpdu.data() + 10, sta, 2000); big.gapSec = 0.001;
        c.frames.push_back(big);
        const WifiTelemetry t = run(c, 0.3);
        printf("  clock +25 ppm: %zu APs, %llu ok, %llu bad, %llu data\n", t.aps.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.perType[2]);
        twoAps(t, "clock");
        CHECK(t.blocksBad == 0 && t.perType[2] >= 10, "clock: %llu bad, %llu data", (unsigned long long)t.blocksBad, (unsigned long long)t.perType[2]);
    }
    {   // another radio rate (resampled to 20 Msps)
        WifiGenConfig c;
        c.rate = 25e6;
        c.snrDb = 25;
        c.frames = wifiTestSchedule();
        const WifiTelemetry t = run(c, 0.26);
        printf("  25 Msps: %zu APs, %llu ok, %llu bad\n", t.aps.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        twoAps(t, "25 Msps");
    }
    {   // noise alone: nothing
        WifiGenConfig c;
        c.snrDb = 0;
        c.amp = 0.25;
        const WifiTelemetry t = run(c, 1.0);
        printf("  noise: %zu APs, %llu ok, %llu SFD hits, %llu headers\n", t.aps.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.sfdHits, (unsigned long long)t.headersOk);
        CHECK(t.aps.empty() && t.blocksOk == 0 && t.headersOk == 0, "noise decoded something");
    }
    {   // too slow a rate: not ready, no crash
        WifiReceiver r;
        r.configure(10e6);
        std::vector<cf32> buf(10000, cf32(0.1f, 0));
        r.feed(buf.data(), buf.size());
        CHECK(!r.ready(), "10 Msps ready");
    }
    printf("  %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    printf(fails ? "wifi rx: %d FAILED\n" : "wifi rx: all passed\n", fails);
    return fails ? 1 : 0;
}
