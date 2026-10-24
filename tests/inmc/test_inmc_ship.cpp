// Inmarsat-C packets decoded after Scytale-C (GPL-3): EGC areas (NAVAREA, coastal, rectangle, circle), the station list (0xAB), the
// announcement (0x81), the logical channel clear (0x27) and messages to ships (0xAA), single and across a multiframe packet.
// The 0x27 and 0x81 packets are recorded ones quoted in Scytale-C's source, with their check bytes; the station list is the quoted
// one with its check bytes recomputed (the quote stops after the first check byte).
#include "dect2/inmc/inmc_pkt.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::inmc;
#include "../common/test_util.h"

static void put(uint8_t* f, int& pos, const std::vector<uint8_t>& p) { memcpy(f + pos, p.data(), p.size()); pos += (int)p.size(); }

static std::vector<uint8_t> hex(const char* s) {
    std::vector<uint8_t> v;
    for (; s[0] && s[1]; s += 2) { unsigned x; sscanf(s, "%2x", &x); v.push_back((uint8_t)x); }
    return v;
}

static InmcTelemetry parse(const std::vector<std::vector<uint8_t>>& packets, FrameParser& fp, int64_t t = 1700000000) {
    uint8_t f[kFrameBytes] = {};
    int pos = 0;
    for (auto& p : packets) put(f, pos, p);
    fp.parseFrame(f, t);
    InmcTelemetry tel;
    fp.fill(tel);
    return tel;
}

int main() {
    // areas
    {
        InmcMessage m;
        decodeArea(0x31, {0x0B, 0x00, 0x00}, m);
        CHECK(m.areaKind == 1 && m.areaNo == 11 && m.areaText == "NAVAREA/METAREA XI, Japan", "navarea: %s", m.areaText.c_str());
        decodeArea(0x13, {0x01, 0x41, 0x41, 0x00, 0x00}, m);
        CHECK(m.areaKind == 2 && m.areaNo == 1 && m.areaText.find("Coastal area I, United Kingdom") == 0, "coastal: %s", m.areaText.c_str());
        // rectangle: 10 S 20 W, 30 nm north and 60 nm east
        decodeArea(0x04, {0x80 | 10, 20, 0x80 | 30, 60, 0, 0}, m);
        CHECK(m.areaKind == 3 && m.lat == -10 && m.lon == -20 && m.extNorthNm == 30 && m.extEastNm == 60, "rectangle %.0f %.0f %d %d", m.lat, m.lon, m.extNorthNm, m.extEastNm);
        CHECK(m.areaText.find("10S 20W, 30 nm north and 60 nm east") != std::string::npos, "rectangle text: %s", m.areaText.c_str());
        // the centre: 15 nm north (0.25 degree) and 30 nm east of the corner
        CHECK(m.areaText.find("centre -9.75, -19.49") != std::string::npos, "rectangle centre: %s", m.areaText.c_str());
        // circle: 45 N 10 E, radius 300 nm
        decodeArea(0x14, {45, 10, 0x01, 0x2C, 0, 0}, m);
        CHECK(m.areaKind == 4 && m.lat == 45 && m.lon == 10 && m.radiusNm == 300 && m.areaText == "Circle: centre 45N 10E, radius 300 nm", "circle: %s", m.areaText.c_str());
        decodeArea(0x44, {0x80 | 33, 151, 0x80, 0x64, 0, 0}, m);
        CHECK(m.areaKind == 4 && m.lat == -33 && m.lon == -151 && m.radiusNm == 100, "circle south-west: %s", m.areaText.c_str());
        decodeArea(0x02, {1, 2, 3, 4}, m);
        CHECK(m.areaKind == 0 && m.areaText.empty(), "FleetNET has no area");
        CHECK(romanNumeral(21) == "XXI" && romanNumeral(4) == "IV" && romanNumeral(9) == "IX", "roman numerals");
    }
    // an EGC message to a circular area: the parser fills the area
    {
        EgcPacket p;
        p.service = 0x14; p.msgId = 77; p.packetNo = 1; p.priority = 3;
        p.address = {0xC2, 45, 10, 0x01, 0x2C, 0, 0};
        const std::string text = "MAYDAY RELAY";
        p.payload.assign(text.begin(), text.end());
        FrameParser fp;
        const auto t = parse({buildEgc(p)}, fp);
        CHECK(t.messages.size() == 1 && t.messages[0].areaKind == 4 && t.messages[0].radiusNm == 300 && t.messages[0].text == text,
              "egc with area: %zu %s", t.messages.size(), t.messages.empty() ? "" : t.messages[0].areaText.c_str());
    }
    // recorded packets: the check bytes agree with ours
    {
        const auto clr = hex("276A49F6551AFBC6");
        CHECK(packetCheckOk(clr.data(), (int)clr.size()), "recorded 0x27 check bytes");
        const auto an = hex("8111EB1DB7442E3E0087017F080007032E3187");
        CHECK(packetCheckOk(an.data(), (int)an.size()), "recorded 0x81 check bytes");
        const auto an2 = hex("8111DDCF43552E5E005205DBB9000601101785");
        CHECK(packetCheckOk(an2.data(), (int)an2.size()), "recorded 0x81 check bytes (2)");
        FrameParser fp;
        const auto t = parse({an, an2}, fp);
        CHECK(t.announcements.size() == 2, "announcements %zu", t.announcements.size());
        const auto& a = t.announcements[1];   // newest first: the first one sent is second
        CHECK(a.mesId == 0xEB1DB7 && a.sat == 1 && a.lesId == 4 && a.logicalChannel == 0x87 && a.presentation == 7, "announcement fields %d %d %d %d %d",
              a.mesId, a.sat, a.lesId, a.logicalChannel, a.presentation);
        CHECK(std::fabs(a.downlinkMhz - ((0x2E3E - 8000) * 0.0025 + 1530)) < 1e-9, "announcement downlink %.4f", a.downlinkMhz);
        CHECK(a.lesName == "Vizada-Telenor, Norway", "announcement station %s", a.lesName.c_str());
    }
    // the recorded station list: Burum-2 on 1539.555 MHz
    {
        auto ab = hex("AB28BF0601F8F7F9FFFF02F8FF792E2E03F8FF792E6204F8FFF92E320CF8FFF92E3A15F8FFF92E22");
        ab.resize(ab.size() + 2);
        packetCheckSet(ab.data(), (int)ab.size());
        CHECK(ab.size() == 42 && ab[40] == 0x78, "station list length and first check byte (0x78 as recorded): %zu %02X", ab.size(), ab[40]);
        FrameParser fp;
        const auto t = parse({ab}, fp);
        CHECK(t.stations.size() == 6, "stations %zu", t.stations.size());
        if (t.stations.size() == 6) {
            CHECK(t.stations[1].lesId == 2 && std::fabs(t.stations[1].downlinkMhz - 1539.555) < 1e-6, "station 2: %d %.3f", t.stations[1].lesId, t.stations[1].downlinkMhz);
            CHECK(t.stations[2].lesId == 3 && std::fabs(t.stations[2].downlinkMhz - 1539.685) < 1e-6, "station 3: %.3f", t.stations[2].downlinkMhz);
            CHECK(t.stations[5].lesId == 21 && std::fabs(t.stations[5].downlinkMhz - 1539.525) < 1e-6, "station 21: %.3f", t.stations[5].downlinkMhz);
            CHECK(t.stations[3].services == 0xFFF9 && !t.stations[3].servicesText.empty(), "services");
        }
        std::vector<StationEntry> se(2);
        se[1].les = 12; se[1].downlinkMhz = 1539.585;
        FrameParser fp2;
        const auto t2 = parse({buildStationList(se)}, fp2);
        CHECK(t2.stations.size() == 2 && t2.stations[1].lesId == 12 && std::fabs(t2.stations[1].downlinkMhz - 1539.585) < 1e-6, "built station list");
    }
    // a message to a ship: announcement, packets 1 to 3 over two frames, then the channel clear
    {
        const std::string text = "MASTER\r\nPLS CONFIRM ETA ROTTERDAM 0600 UTC.\r\nCARGO DOCS SENT BY EMAIL.\r\nBRGDS OPS";
        std::vector<uint8_t> ia5;
        for (char c : text) ia5.push_back((uint8_t)c | 0x80);      // as sent: the high bit set, as in the recorded packets
        auto part = [&](int i) { return std::vector<uint8_t>(ia5.begin() + i * 30, ia5.begin() + std::min<size_t>(ia5.size(), (size_t)(i + 1) * 30)); };
        FrameParser fp;
        auto t = parse({buildAnnouncement(4567890, 1, 4, 1537.70, 9, 0), buildShipMessage(1, 4, 9, 1, part(0)), buildShipMessage(1, 4, 9, 2, part(1))}, fp);
        CHECK(t.messages.size() == 1, "one message so far: %zu", t.messages.size());
        if (!t.messages.empty()) {
            const auto& m = t.messages[0];
            CHECK(m.kind == 3 && m.mesId == 4567890 && m.logicalChannel == 9 && m.packets == 2 && !m.complete, "ship message fields %d %d %d %d", m.kind, m.mesId, m.packets, m.complete);
            CHECK(m.serviceText == "Message to ship 4567890, channel 9", "service text %s", m.serviceText.c_str());
        }
        t = parse({buildShipMessage(1, 4, 9, 3, part(2)), buildChannelClear(4567890, 1, 4, 9)}, fp, 1700000009);
        CHECK(t.messages.size() == 1 && t.messages[0].complete && t.messages[0].packets == 3, "complete after the channel clear");
        const std::string want = "MASTER\nPLS CONFIRM ETA ROTTERDAM 0600 UTC.\nCARGO DOCS SENT BY EMAIL.\nBRGDS OPS";
        CHECK(!t.messages.empty() && t.messages[0].text == want, "text [%s]", t.messages.empty() ? "" : t.messages[0].text.c_str());
        // the same channel again: a new message
        t = parse({buildShipMessage(1, 4, 9, 1, part(0))}, fp, 1700000020);
        CHECK(t.messages.size() == 2, "a second message on the channel: %zu", t.messages.size());
        // an EGC message with the same number as the channel stays separate
        EgcPacket e;
        e.service = 0x00; e.msgId = 9; e.packetNo = 1;
        e.address.assign((size_t)addressLength(0x00), 0x01);
        e.payload = {'H', 'I'};
        t = parse({buildEgc(e)}, fp, 1700000030);
        CHECK(t.messages.size() == 3, "EGC and ship messages kept apart: %zu", t.messages.size());
    }
    // a message to a ship without an announcement: control codes make it binary, else it is text
    {
        FrameParser fp;
        auto t = parse({buildShipMessage(0, 2, 3, 1, {'O', 'K', ' ', 'T', 'H', 'A', 'N', 'K', 'S'})}, fp);
        CHECK(t.messages.size() == 1 && t.messages[0].mesId == -1 && t.messages[0].text == "OK THANKS", "unannounced text [%s]",
              t.messages.empty() ? "" : t.messages[0].text.c_str());
        FrameParser fp2;
        t = parse({buildShipMessage(0, 2, 3, 1, {0x01, 0x10, 0x20, 0x30, 0x7F})}, fp2);
        CHECK(t.messages.size() == 1 && t.messages[0].text == "[binary, 5 bytes]", "binary [%s]", t.messages.empty() ? "" : t.messages[0].text.c_str());
    }
    // a message to a ship carried in a multiframe packet (0xBD then 0xBE)
    {
        std::vector<uint8_t> body(70, 'A');
        auto inner = buildShipMessage(2, 3, 5, 1, body);
        inner.resize(inner.size() - 2);       // the assembled packet has no check bytes of its own
        const size_t cut = 40;
        std::vector<uint8_t> bd = {0xBD, 0};
        bd.insert(bd.end(), inner.begin(), inner.begin() + cut);
        bd.resize(bd.size() + 2);
        bd[1] = (uint8_t)(bd.size() - 2);
        packetCheckSet(bd.data(), (int)bd.size());
        std::vector<uint8_t> be = {0xBE, 0};
        be.insert(be.end(), inner.begin() + cut, inner.end());
        be.resize(be.size() + 2);
        be[1] = (uint8_t)(be.size() - 2);
        packetCheckSet(be.data(), (int)be.size());
        FrameParser fp;
        parse({bd}, fp);
        const auto t = parse({be}, fp, 1700000009);
        CHECK(t.messages.size() == 1 && t.messages[0].kind == 3 && t.messages[0].text == std::string(70, 'A'), "multiframe ship message: %zu",
              t.messages.size());
    }
    printf(fails ? "FAILED\n" : "inmc_ship ok\n");
    return fails ? 1 : 0;
}
