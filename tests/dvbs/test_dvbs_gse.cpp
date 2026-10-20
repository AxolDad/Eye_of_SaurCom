// IP over DVB-S2 through the whole chain: the generator sends UDP/IP packets in GSE on BBFRAMEs (every GSE variant, fragmented PDUs, ACM labels), the receiver
// decodes the signal and reports the IP traffic (hosts, ports, protocols, volume, the terminals of the outbound) and writes a PCAP file that holds the packets
// that were sent. A transport stream is not involved.
#include "dect2/dvbs/dvbs_gen.h"
#include "dect2/dvbs/dvbs_ip.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/dvbs/dvbs_testkit.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
using namespace dect2;
using namespace dect2::dvbs;
#include "../common/test_util.h"

int main() {
    const dvbsip::Variant variants[2] = {dvbsip::kVariants[0], dvbsip::kVariants[2]};
    for (const dvbsip::Variant& var : variants) {
        DvbsSignalConfig c;
        c.tx.standard = 2; c.tx.mod = kQpsk; c.tx.rate = 5; c.tx.symbolRate = 2e6;
        c.sampleRate = 4e6; c.snrDb = s2QefEsN0(kQpsk, 5, false) + 4.0;
        dvbsip::GseEncapsulator enc(var);
        uint32_t n = 0;
        std::vector<std::vector<uint8_t>> sent;
        c.tx.gseField = [&](size_t bytes) {
            while (enc.queued() < 8) {
                const uint8_t src[4] = {10, 0, (uint8_t)(n % 3), (uint8_t)(n % 5)}, dst[4] = {192, 168, 7, 1};
                static const size_t lens[4] = {60, 900, 3000, 200};
                std::vector<uint8_t> payload(lens[n % 4]);
                for (size_t i = 0; i < payload.size(); i++) payload[i] = (uint8_t)(n * 31 + i * 7);
                auto ip = dvbsip::buildUdpV4(src, dst, 4000, (uint16_t)(5000 + n % 4), payload, true);
                dvbsip::Label l; l.type = 0; l.b = {0, 1, 2, 3, 4, (uint8_t)(n % 3)};
                enc.push(0x0800, l, ip);
                sent.push_back(ip);
                n++;
            }
            return enc.fill(bytes);
        };
        DvbsSignal sig(c);
        DvbsReceiver rx;
        rx.setBlocking(true);
        rx.configure(c.sampleRate);
        const std::string path = "/tmp/dvbs_gse_test.pcap";
        rx.setIpPcap(path);
        std::vector<cf32> buf(65536);
        const size_t total = (size_t)(4.0 * c.sampleRate);
        for (size_t done = 0; done < total; done += buf.size()) {
            sig.generate(buf.data(), buf.size());
            for (auto& v : buf) { auto q = [](float x) { return std::round(std::min(127.f, std::max(-128.f, x * 128.f))) / 128.f; }; v = cf32(q(v.real()), q(v.imag())); }
            rx.feed(buf.data(), buf.size());
        }
        rx.flush();
        DvbsTelemetry t;
        CHECK(rx.telemetry(t, 0), "telemetry");
        rx.setIpPcap("");
        const DvbsIpTel& ip = t.ip;
        printf("  %s: %s, %llu packets, %zu flows, %zu labels\n", var.label(), ip.source.c_str(), (unsigned long long)ip.packets, ip.flows.size(), ip.labels.size());
        CHECK(t.standard == 2 && t.gseFrames > 50, "S2 with GSE frames: %llu", (unsigned long long)t.gseFrames);
        CHECK(!t.tsLock && t.packets == 0, "no transport stream");
        CHECK(ip.active && ip.packets > 100, "IP packets %llu", (unsigned long long)ip.packets);
        CHECK(ip.source.find(var.label()) != std::string::npos, "source names the variant: %s", ip.source.c_str());
        CHECK(ip.ipv4 == ip.packets && ip.ipv6 == 0, "all IPv4");
        CHECK(ip.protocols.size() == 1 && ip.protocols[0].first == "UDP", "UDP only");
        std::set<std::string> dports;
        for (const auto& p : ip.udpPorts) dports.insert(p.first);
        CHECK(dports == std::set<std::string>({"5000", "5001", "5002", "5003"}), "destination ports");
        CHECK(!ip.flows.empty() && ip.flows[0].dst == "192.168.7.1" && ip.flows[0].protocol == "UDP" && !ip.flows[0].encrypted, "top flow");
        CHECK(ip.encryptedFlows == 0 && !ip.otherEncrypted, "nothing encrypted");
        CHECK(ip.labels.size() == 3 && ip.labels[0].ip > 10 && ip.labels[0].modcods.find("QPSK") == 0, "three terminals, sent in QPSK: %s", ip.labels.empty() ? "none" : ip.labels[0].modcods.c_str());
        
        // the PCAP file holds sent packets
        std::ifstream f(path, std::ios::binary);
        std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(file.size() > 24 && file[20] == 101, "pcap file");
        size_t at = 24, count = 0, match = 0;
        while (at + 16 <= file.size()) {
            const uint32_t incl = file[at + 8] | file[at + 9] << 8 | file[at + 10] << 16 | (uint32_t)file[at + 11] << 24;
            at += 16;
            if (at + incl > file.size()) break;
            std::vector<uint8_t> pk(file.begin() + (long)at, file.begin() + (long)(at + incl));
            at += incl;
            count++;
            for (const auto& s : sent) if (s == pk) { match++; break; }
        }
        CHECK(count > 100 && match == count, "pcap: %zu packets, %zu of them exactly as sent", count, match);
        std::remove(path.c_str());
    }
    // MPE and ULE handed over as transport stream packets (what the engine does with the receiver's output)
    {
        DvbsReceiver rx;
        rx.setBlocking(true);
        rx.configure(4e6);
        uint8_t cc = 0, cu = 0;
        const uint8_t mac[6] = {1, 0, 0x5E, 1, 1, 1}, src[4] = {10, 1, 1, 1}, dst[4] = {239, 1, 1, 1};
        std::vector<std::vector<uint8_t>> sndus;
        for (int k = 0; k < 20; k++) {
            auto ip = dvbsip::buildUdpV4(src, dst, 5000, 6000, std::vector<uint8_t>(200, (uint8_t)k), true);
            for (auto& p : dvbsip::packetizeSection(0x0FA0, cc, dvbsip::buildMpeSection(mac, ip))) rx.feedTs(p.data(), 1);
            sndus.push_back(dvbsip::buildUleSndu(0x0800, ip));
        }
        for (auto& p : dvbsip::packetizeUle(0x0123, cu, sndus)) rx.feedTs(p.data(), 1);
        std::vector<cf32> z(65536);
        rx.feed(z.data(), z.size());
        rx.flush();
        DvbsTelemetry t;
        CHECK(rx.telemetry(t, 0), "telemetry");
        printf("  TS: %s, %llu packets (MPE %llu, ULE %llu)\n", t.ip.source.c_str(), (unsigned long long)t.ip.packets, (unsigned long long)t.ip.mpePackets, (unsigned long long)t.ip.ulePackets);
        CHECK(t.ip.mpePackets == 20 && t.ip.ulePackets == 18 && t.ip.packets == 38, "MPE 20 and ULE 18 (two go to learning the PID): %llu %llu", (unsigned long long)t.ip.mpePackets, (unsigned long long)t.ip.ulePackets);
        CHECK(t.ip.source.find("MPE") != std::string::npos && t.ip.source.find("ULE") != std::string::npos, "source: %s", t.ip.source.c_str());
        CHECK(!t.ip.flows.empty() && t.ip.flows[0].dst == "239.1.1.1" && t.ip.flows[0].dport == 6000, "multicast flow");
    }
    return testSummary("dvbs gse");
}
