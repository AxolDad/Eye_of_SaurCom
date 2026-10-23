#include "dect2/inmc/inmc_pkt.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include "dect2/common/timecompat.h"

namespace dect2 {
namespace inmc {

std::string satName(int sat) {
    switch (sat) {
    case 0: return "Atlantic Ocean Region West (AOR-W)";
    case 1: return "Atlantic Ocean Region East (AOR-E)";
    case 2: return "Pacific Ocean Region (POR)";
    case 3: return "Indian Ocean Region (IOR)";
    case 9: return "All ocean regions of the station";
    default: return "Unknown";
    }
}

std::string lesName(int sat, int les) {
    switch (les + sat * 100) {   // inmarsatc getLesName / stdcdec get_les_name; the value is region * 100 + station
    case 1: case 101: case 201: case 301: return "Vizada-Telenor, USA";
    case 2: case 102: case 302: return "Stratos Global (Burum-2), Netherlands";
    case 202: return "Stratos Global (Auckland), New Zealand";
    case 3: case 103: case 203: case 303: return "KDDI, Japan";
    case 4: case 104: case 204: case 304: return "Vizada-Telenor, Norway";
    case 44: case 144: case 244: case 344: return "NCS";
    case 105: case 335: return "Telecom Italia";
    case 305: case 120: return "OTESTAT, Greece";
    case 306: return "VSNL, India";
    case 110: case 310: return "Turk Telecom, Turkey";
    case 211: case 311: return "Beijing MCN, China";
    case 12: case 112: case 212: case 312: return "Stratos Global (Burum), Netherlands";
    case 114: return "Embratel, Brazil";
    case 116: case 316: return "Telekomunikacja Polska, Poland";
    case 117: case 217: case 317: return "Morsviazsputnik, Russia";
    case 21: case 121: case 221: case 321: return "Vizada (FT), France";
    case 127: case 327: return "Bezeq, Israel";
    case 210: case 328: return "Singapore Telecom, Singapore";
    case 330: return "VISHIPEL, Vietnam";
    default: return "";
    }
}

std::string serviceName(int code) {
    switch (code) {
    case 0x00: return "System, all ships (general call)";
    case 0x02: return "FleetNET, group call";
    case 0x04: return "SafetyNET, warning to a rectangular area";
    case 0x11: return "System, Inmarsat system message";
    case 0x13: return "SafetyNET, coastal warning";
    case 0x14: return "SafetyNET, shore-to-ship distress alert to a circular area";
    case 0x23: return "System, EGC system message";
    case 0x24: return "SafetyNET, warning to a circular area";
    case 0x31: return "SafetyNET, NAVAREA/METAREA warning or forecast";
    case 0x33: return "System, download group identity";
    case 0x34: return "SafetyNET, SAR coordination to a rectangular area";
    case 0x44: return "SafetyNET, SAR coordination to a circular area";
    case 0x72: return "FleetNET, chart correction service";
    case 0x73: return "SafetyNET, chart correction service for fixed areas";
    default: return "Unknown service";
    }
}

int serviceKind(int code) {
    switch (code) {
    case 0x04: case 0x13: case 0x14: case 0x24: case 0x31: case 0x34: case 0x44: case 0x73: return 1;
    case 0x02: case 0x72: return 2;
    default: return 0;
    }
}

const char* priorityName(int p) {
    static const char* n[] = {"Routine", "Safety", "Urgency", "Distress"};
    return n[p & 3];
}

int addressLength(int code) {
    switch (code) {
    case 0x00: return 3;
    case 0x11: case 0x31: return 4;
    case 0x02: case 0x72: return 5;
    case 0x13: case 0x23: case 0x33: case 0x73: return 6;
    case 0x04: case 0x14: case 0x24: case 0x34: case 0x44: return 7;
    default: return 3;
    }
}

const char* descriptorName(uint8_t d) {
    switch (d) {
    case 0x27: return "Logical channel clear";
    case 0x2A: return "Inbound message ack";
    case 0x08: return "Acknowledgement request";
    case 0x6C: return "Signalling channel";
    case 0x7D: return "Bulletin board";
    case 0x81: return "Announcement";
    case 0x83: return "Logical channel assignment";
    case 0x91: return "Distress alert ack";
    case 0x92: return "Login ack";
    case 0x9A: return "Enhanced data report ack";
    case 0xA0: return "Distress test request";
    case 0xA3: return "Individual poll";
    case 0xA8: return "Confirmation";
    case 0xAA: return "Message";
    case 0xAB: return "LES list";
    case 0xAC: return "Request status";
    case 0xAD: return "Test result";
    case 0xB1: return "EGC double header 1";
    case 0xB2: return "EGC double header 2";
    case 0xBD: return "Multiframe packet";
    case 0xBE: return "Multiframe continuation";
    default: return "Other";
    }
}

std::string servicesText(unsigned m) {
    static const char* n[16] = {"Distress alerting", "SafetyNET", "Inmarsat-C", "Store and forward", "Half duplex", "Full duplex",
                                "Closed network", "FleetNET", "Prefix SF", "Land mobile alerting", "Aero-C", "ITA2", "Data", "Basic X.400",
                                "Enhanced X.400", "Low power CMES"};
    std::string s;
    for (int i = 0; i < 16; i++)
        if (m & (0x8000u >> i)) { if (!s.empty()) s += ", "; s += n[i]; }
    return s;
}

std::string frameTimeText(uint32_t f) {
    const double s = f * kFrameSeconds;
    const int h = (int)(s / 3600), m = (int)(std::fmod(s, 3600) / 60);
    char b[48];
    snprintf(b, sizeof b, "%d:%02d:%04.1f", h, m, std::fmod(s, 60));
    return b;
}

std::string utcText(int64_t t) { return dect2::utcClockText(t); }

double downlinkMhz(uint16_t ch) { return (ch - 8000) * 0.0025 + 1530.0; }
uint16_t downlinkChannel(double mhz) { return (uint16_t)(std::lround((mhz - 1530.0) / 0.0025) + 8000); }

std::string navAreaCoordinator(int a) {
    static const char* c[22] = {"", "United Kingdom", "France", "Spain", "United States", "Brazil", "Argentina", "South Africa", "India",
                                "Pakistan", "Australia", "Japan", "United States", "Russia", "New Zealand", "Chile", "Peru", "Canada",
                                "Canada", "Norway", "Russia", "Russia"};
    return a >= 1 && a <= 21 ? c[a] : "";
}

std::string romanNumeral(int n) {
    static const struct { int v; const char* s; } t[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
                                                        {50, "L"}, {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}};
    std::string s;
    for (auto& e : t) while (n >= e.v) { s += e.s; n -= e.v; }
    return s;
}

bool looksBinary(const std::vector<uint8_t>& b) {
    for (uint8_t x : b) {
        const uint8_t c = x & 0x7F;
        if (c == 0x01 || c == 0x03 || (c >= 0x05 && c <= 0x08) || c == 0x0B || c == 0x0C || (c >= 0x0E && c <= 0x1A) || (c >= 0x1C && c <= 0x1F) ||
            c == '$')
            return true;
    }
    return false;
}

// a point at a distance along a bearing from another (great circle, sphere of 6371.01 km), as Scytale-C's FindPointAtDistanceFrom
static void pointAt(double lat, double lon, double bearing, double km, double& olat, double& olon) {
    const double r = 6371.01, d = km / r, la = lat * M_PI / 180, lo = lon * M_PI / 180;
    const double la2 = std::asin(std::sin(la) * std::cos(d) + std::cos(la) * std::sin(d) * std::cos(bearing));
    const double lo2 = lo + std::atan2(std::sin(bearing) * std::sin(d) * std::cos(la), std::cos(d) - std::sin(la) * std::sin(la2));
    olat = la2 * 180 / M_PI;
    olon = lo2 * 180 / M_PI;
}

void decodeArea(int code, const std::vector<uint8_t>& a, InmcMessage& m) {
    char b[200];
    m.areaKind = 0;
    m.areaText.clear();
    switch (code) {
    case 0x00: case 0x31: case 0x13: {
        if (a.empty()) return;
        m.areaNo = a[0];
        m.areaKind = code == 0x13 ? 2 : 1;
        const std::string who = navAreaCoordinator(m.areaNo);
        snprintf(b, sizeof b, "%s %s%s%s", code == 0x13 ? "Coastal area" : "NAVAREA/METAREA", romanNumeral(m.areaNo).c_str(),
                 who.empty() ? "" : ", ", who.c_str());
        m.areaText = b;
        return;
    }
    case 0x04: case 0x34: case 0x14: case 0x24: case 0x44: {
        if (a.size() < 4) return;
        // 1 bit south, 7 bits latitude; 8 bits longitude; 1 bit west, then 7 bits north and 8 bits east (rectangle) or 15 bits radius
        m.lat = (a[0] & 0x7F) * ((a[0] & 0x80) ? -1 : 1);
        m.lon = a[1] * ((a[2] & 0x80) ? -1 : 1);
        const char* ns = m.lat < 0 ? "S" : "N";
        const char* ew = m.lon < 0 ? "W" : "E";
        if (code == 0x04 || code == 0x34) {
            m.areaKind = 3;
            m.extNorthNm = a[2] & 0x7F;
            m.extEastNm = a[3];
            double clat, clon, hlat, hlon;
            pointAt(m.lat, m.lon, M_PI / 2, m.extEastNm / 2.0 * 1.852, hlat, hlon);
            pointAt(hlat, hlon, 0, m.extNorthNm / 2.0 * 1.852, clat, clon);
            snprintf(b, sizeof b, "Rectangle: south-west corner %.0f%s %.0f%s, %d nm north and %d nm east (centre %.2f, %.2f)",
                     std::fabs(m.lat), ns, std::fabs(m.lon), ew, m.extNorthNm, m.extEastNm, clat, clon);
        } else {
            m.areaKind = 4;
            m.radiusNm = (a[2] & 0x7F) << 8 | a[3];
            snprintf(b, sizeof b, "Circle: centre %.0f%s %.0f%s, radius %d nm", std::fabs(m.lat), ns, std::fabs(m.lon), ew, m.radiusNm);
        }
        m.areaText = b;
        return;
    }
    default: return;
    }
}

// ITA2 tables as in the table of the open decoder (from the baudot project); 0x1F shifts to letters, 0x1B to figures
std::string ita2ToText(const std::vector<uint8_t>& codes) {
    static const char ltrs[32] = {0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K',
                                  'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
    static const char figs[32] = {0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', 0, '4', 0, ',', '!', ':', '(',
                                  '5', '+', ')', '2', '$', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};
    std::string s;
    bool fig = false;
    for (uint8_t c : codes) {
        c &= 31;
        if (c == 0x1F) { fig = false; continue; }
        if (c == 0x1B) { fig = true; continue; }
        const char ch = fig ? figs[c] : ltrs[c];
        if (ch && ch != '\r') s += ch;
    }
    return s;
}

std::vector<uint8_t> textToIta2(const std::string& text) {
    std::vector<uint8_t> o;
    bool fig = false, known = false;
    for (char c : text) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        int code = -1; bool f = false;
        for (int i = 1; i < 32 && code < 0; i++) {
            static const char L[32] = {0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K', 'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
            if (L[i] == c) code = i;
        }
        if (code < 0)
            for (int i = 1; i < 32 && code < 0; i++) {
                static const char F[32] = {0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', 0, '4', 0, ',', '!', ':', '(', '5', '+', ')', '2', '$', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};
                if (F[i] == c) { code = i; f = true; }
            }
        if (code < 0) continue;
        const bool shared = (c == ' ' || c == '\n' || c == '\r');
        if (!shared && (!known || f != fig)) { o.push_back(f ? 0x1B : 0x1F); fig = f; known = true; }
        o.push_back((uint8_t)code);
    }
    return o;
}

std::string decodeText(int pres, const std::vector<uint8_t>& b) {
    if (pres == 6) return ita2ToText(b);
    if (pres == 7) { char t[48]; snprintf(t, sizeof t, "[binary, %zu bytes]", b.size()); return t; }
    std::string s;
    for (size_t i = 0; i < b.size(); i++) {
        const char c = (char)(b[i] & 0x7F);
        if (c == 0x03) break;                       // end of text
        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || c >= 0x20) s += c;
    }
    return s;
}

// ---------------------------------------------------------------- builders
std::vector<uint8_t> buildBulletinBoard(const BulletinBoard& b) {
    std::vector<uint8_t> p(14, 0);
    p[0] = 0x7D;
    p[1] = (uint8_t)b.networkVersion;
    p[2] = (uint8_t)(b.frameNo >> 8); p[3] = (uint8_t)b.frameNo;
    p[4] = (uint8_t)(b.signallingChannel << 2);
    p[5] = (uint8_t)(((b.count / 2) & 15) << 4);
    p[6] = (uint8_t)((b.channelType << 5) | ((b.local & 7) << 2));
    p[7] = (uint8_t)((b.sat << 6) | (b.les & 63));
    p[8] = b.status;
    p[9] = (uint8_t)(b.services >> 8); p[10] = (uint8_t)b.services;
    p[11] = (uint8_t)b.randomInterval;
    packetCheckSet(p.data(), 14);
    return p;
}

std::vector<uint8_t> buildSignalling(uint8_t sv, double uplinkMhz, const uint8_t slots[28]) {
    std::vector<uint8_t> p(13, 0);
    p[0] = 0x6C;
    p[1] = sv;
    const int ch = (int)std::lround((uplinkMhz - 1626.5) / 0.0025) + 6000;
    p[2] = (uint8_t)(ch >> 8); p[3] = (uint8_t)ch;
    for (int i = 0; i < 28; i++) p[4 + i / 4] |= (uint8_t)((slots[i] & 3) << (6 - 2 * (i % 4)));
    packetCheckSet(p.data(), 13);
    return p;
}

std::vector<uint8_t> buildEgc(const EgcPacket& e) {
    const int al = addressLength(e.service);
    const int len = 8 + al + (int)e.payload.size() + 2;
    std::vector<uint8_t> p((size_t)len, 0);
    p[0] = e.desc;
    p[1] = (uint8_t)(len - 2);
    p[2] = e.service;
    p[3] = (uint8_t)((e.continuation ? 0x80 : 0) | ((e.priority & 3) << 5) | (e.repetition & 31));
    p[4] = (uint8_t)(e.msgId >> 8); p[5] = (uint8_t)e.msgId;
    p[6] = (uint8_t)e.packetNo;
    p[7] = (uint8_t)e.presentation;
    for (int i = 0; i < al && i < (int)e.address.size(); i++) p[8 + i] = e.address[i];
    std::copy(e.payload.begin(), e.payload.end(), p.begin() + 8 + al);
    packetCheckSet(p.data(), len);
    return p;
}

std::vector<uint8_t> buildStationList(const std::vector<StationEntry>& s) {
    const int len = 4 + 6 * (int)s.size() + 2;      // descriptor, length, a byte, count, stations, check bytes
    std::vector<uint8_t> p((size_t)len, 0);
    p[0] = 0xAB;
    p[1] = (uint8_t)(len - 2);
    p[2] = 0xBF;
    p[3] = (uint8_t)s.size();
    for (size_t i = 0; i < s.size(); i++) {
        uint8_t* q = &p[4 + 6 * i];
        q[0] = (uint8_t)((s[i].sat << 6) | (s[i].les & 63));
        q[1] = 0xF8;
        q[2] = (uint8_t)(s[i].services >> 8); q[3] = (uint8_t)s[i].services;
        const uint16_t ch = downlinkChannel(s[i].downlinkMhz);
        q[4] = (uint8_t)(ch >> 8); q[5] = (uint8_t)ch;
    }
    packetCheckSet(p.data(), len);
    return p;
}

std::vector<uint8_t> buildAnnouncement(int mesId, int sat, int les, double mhz, int lcn, int pres) {
    std::vector<uint8_t> p(19, 0);
    p[0] = 0x81;
    p[1] = 17;
    p[2] = (uint8_t)(mesId >> 16); p[3] = (uint8_t)(mesId >> 8); p[4] = (uint8_t)mesId;
    p[5] = (uint8_t)((sat << 6) | (les & 63));
    const uint16_t ch = downlinkChannel(mhz);
    p[6] = (uint8_t)(ch >> 8); p[7] = (uint8_t)ch;
    p[9] = (uint8_t)lcn;
    p[14] = (uint8_t)pres;
    packetCheckSet(p.data(), 19);
    return p;
}

std::vector<uint8_t> buildChannelClear(int mesId, int sat, int les, int lcn) {
    std::vector<uint8_t> p(8, 0);
    p[0] = 0x27;
    p[1] = (uint8_t)(mesId >> 16); p[2] = (uint8_t)(mesId >> 8); p[3] = (uint8_t)mesId;
    p[4] = (uint8_t)((sat << 6) | (les & 63));
    p[5] = (uint8_t)lcn;
    packetCheckSet(p.data(), 8);
    return p;
}

std::vector<uint8_t> buildShipMessage(int sat, int les, int lcn, int packetNo, const std::vector<uint8_t>& payload) {
    const int len = 5 + (int)payload.size() + 2;
    std::vector<uint8_t> p((size_t)len, 0);
    p[0] = 0xAA;
    p[1] = (uint8_t)(len - 2);
    p[2] = (uint8_t)((sat << 6) | (les & 63));
    p[3] = (uint8_t)lcn;
    p[4] = (uint8_t)packetNo;
    std::copy(payload.begin(), payload.end(), p.begin() + 5);
    packetCheckSet(p.data(), len);
    return p;
}

// ---------------------------------------------------------------- parser
static std::atomic<uint64_t> gOrder{0};      // shared by all parsers, so that messages of several channels sort by arrival

void trimMessages(std::vector<InmcMessage>& v, size_t maxCount, size_t budget) {
    std::sort(v.begin(), v.end(), [](const InmcMessage& a, const InmcMessage& b) { return a.order > b.order; });
    if (v.size() > maxCount) v.resize(maxCount);
    for (auto& m : v) {       // older messages lose the end of their text first
        const size_t cost = m.text.size() + 300;
        if (cost > budget) {
            if (m.text.size() > 120) { m.text.resize(120); m.truncated = true; }
            budget = budget > 420 ? budget - 420 : 0;
        } else budget -= cost;
    }
}

FrameParser::FrameParser() { reset(); }

void FrameParser::reset() {
    ncs_ = InmcNcsInfo();
    slots_.clear();
    std::fill(pktOk_, pktOk_ + 256, 0u);
    std::fill(pktBad_, pktBad_ + 256, 0u);
    packetsOk_ = packetsBad_ = 0;
    messageCount_ = frameNo_ = curFrame_ = 0;
    mfp_.clear(); mfpTotal_ = 0;
    stations_.clear();
    announcements_.clear();
}

static int packetLength(const uint8_t* f, int pos, int end) {
    const uint8_t d = f[pos];
    if ((d >> 7) == 0) return (d & 0x0F) + 1;                    // short descriptor
    if ((d >> 6) == 2 && pos + 1 < end) return f[pos + 1] + 2;   // medium descriptor
    return -1;                                                   // long descriptors are not used by the channels decoded here
}

bool FrameParser::parseFrame(const uint8_t* f, int64_t rxUnix) {
    bool bbFirst = false;
    const int end = kFrameBytes - 1;     // the last byte is the flush byte
    int pos = 0;
    while (pos < end) {
        const uint8_t d = f[pos];
        if (d == 0) break;                // no more packets
        const int len = packetLength(f, pos, end);
        if (len < 3 || pos + len > end) { packetsBad_++; pktBad_[d]++; break; }
        const bool ok = packetCheckOk(f + pos, len);
        if (!ok) { packetsBad_++; pktBad_[d]++; pos += len; continue; }
        packetsOk_++; pktOk_[d]++;
        const uint8_t* p = f + pos;
        if (d == 0x7D && len >= 14) {
            if (pos == 0) bbFirst = true;
            ncs_.valid = true;
            ncs_.networkVersion = p[1];
            ncs_.frameNo = (uint32_t)(p[2] << 8 | p[3]);
            frameNo_ = ncs_.frameNo;
            ncs_.frameTime = frameTimeText(ncs_.frameNo);
            ncs_.signallingChannel = p[4] >> 2;
            ncs_.count = (p[5] >> 4) * 2;
            ncs_.channelType = p[6] >> 5;
            static const char* ct[] = {"Reserved", "NCS", "LES TDM", "Joint NCS and TDM", "Stand-by NCS"};
            ncs_.channelTypeName = ncs_.channelType <= 4 ? ct[ncs_.channelType] : "Reserved";
            ncs_.sat = p[7] >> 6;
            ncs_.region = satName(ncs_.sat);
            ncs_.lesId = p[7] & 63;
            ncs_.lesName = lesName(ncs_.sat, ncs_.lesId);
            ncs_.status = p[8];
            std::string st;
            const char* sn[] = {"600 baud", "operational", "in service", "clear", "links open"};
            for (int i = 0; i < 5; i++) if (p[8] & (0x80 >> i)) { if (!st.empty()) st += ", "; st += sn[i]; }
            ncs_.statusText = st;
            ncs_.services = (uint16_t)(p[9] << 8 | p[10]);
            ncs_.servicesText = servicesText(ncs_.services);
            ncs_.randomInterval = p[11];
            curFrame_ = ncs_.frameNo;
        } else if (d == 0x6C && len >= 13) {
            ncs_.signallingUplinkMhz = ((p[2] << 8 | p[3]) - 6000) * 0.0025 + 1626.5;
        } else if ((d == 0xB1 || d == 0xB2) && len >= 10) {
            egc(p, len, rxUnix);
        } else if (d == 0xAA && len >= 8) {
            shipMessage(p, len - 2, rxUnix);
        } else if (d == 0xAB && len >= 6) {
            const int n = p[3];
            if (4 + 6 * n <= len - 2) {
                stations_.clear();
                for (int i = 0; i < n; i++) {
                    const uint8_t* q = p + 4 + 6 * i;
                    InmcStation s;
                    s.sat = q[0] >> 6; s.lesId = q[0] & 63;
                    s.region = satName(s.sat);
                    s.lesName = lesName(s.sat, s.lesId);
                    s.services = (uint16_t)(q[2] << 8 | q[3]);
                    s.servicesText = servicesText(s.services);
                    s.downlinkMhz = downlinkMhz((uint16_t)(q[4] << 8 | q[5]));
                    stations_.push_back(s);
                }
            }
        } else if (d == 0x81 && len >= 17) {
            InmcAnnouncement an;
            an.mesId = p[2] << 16 | p[3] << 8 | p[4];
            an.sat = p[5] >> 6; an.lesId = p[5] & 63;
            an.lesName = lesName(an.sat, an.lesId);
            an.downlinkMhz = downlinkMhz((uint16_t)(p[6] << 8 | p[7]));
            an.logicalChannel = p[9];
            an.presentation = p[14];
            an.rxUnix = rxUnix; an.rxTime = utcText(rxUnix);
            announcements_.insert(announcements_.begin(), an);
            if (announcements_.size() > 50) announcements_.pop_back();
        } else if (d == 0x27 && len >= 8) {
            // the station clears the logical channel: the message on it is complete
            const int sat = p[4] >> 6, les = p[4] & 63, lcn = p[5];
            for (auto& x : slots_)
                if (x.msg.kind == 3 && x.msg.sat == sat && x.msg.lesId == les && x.msg.logicalChannel == lcn && !x.msg.complete) {
                    x.msg.complete = true;
                    x.msg.seen++;
                }
        } else if (d == 0xBD && len >= 6) {
            // multiframe packet: the start of another packet, continued in 0xBE packets of this or the next frames
            const uint8_t inner = p[2];
            int il = -1;
            if ((inner >> 7) == 0) il = (inner & 15) + 1;
            else if ((inner >> 6) == 2) il = p[3] + 2;
            mfp_.assign(p + 2, p + len - 2);
            mfpTotal_ = il > 0 ? (size_t)il : 0;
        } else if (d == 0xBE && len >= 4 && mfpTotal_) {
            mfp_.insert(mfp_.end(), p + 2, p + len - 2);
            if (mfp_.size() + 2 >= mfpTotal_) {
                mfp_.resize(mfpTotal_, 0);      // the assembled packet has no check bytes of its own
                if ((mfp_[0] == 0xB1 || mfp_[0] == 0xB2) && mfp_.size() >= 10) egc(mfp_.data(), (int)mfp_.size(), rxUnix);
                else if (mfp_[0] == 0xAA && mfp_.size() >= 8) shipMessage(mfp_.data(), (int)mfp_.size() - 2, rxUnix);   // its last two bytes stand for the check bytes
                mfp_.clear(); mfpTotal_ = 0;
            }
        }
        pos += len;
    }
    return bbFirst;
}

void FrameParser::egc(const uint8_t* p, int len, int64_t rxUnix) {
    const uint8_t svc = p[2];
    const int al = addressLength(svc);
    const int hdr = 8 + al;
    if (hdr > len - 2) return;
    const bool cont = (p[3] & 0x80) != 0;
    const int prio = (p[3] >> 5) & 3, rep = p[3] & 31;
    const uint16_t id = (uint16_t)(p[4] << 8 | p[5]);
    const int pno = p[6], pres = p[7];
    const int key = pno * 2 + (p[0] == 0xB2 ? 1 : 0);
    Slot* s = nullptr;
    for (auto& x : slots_) if (x.msg.kind != 3 && x.msg.id == id && x.msg.serviceCode == svc) { s = &x; break; }
    if (!s) {
        s = newSlot();
        s->msg.id = id; s->msg.serviceCode = svc;
        s->msg.kind = serviceKind(svc);
        s->msg.serviceText = serviceName(svc);
        s->msg.frameNo = curFrame_;
        s->msg.rxUnix = rxUnix;
        s->msg.rxTime = utcText(rxUnix);
        messageCount_++;
    }
    if (pno == 1 && p[0] == 0xB1 && s->msg.complete) {   // the next broadcast of a message we have: start a new pass
        s->parts.clear(); s->lastKey = -1; s->msg.packets = 0;
        s->msg.rxUnix = rxUnix; s->msg.rxTime = utcText(rxUnix); s->msg.frameNo = curFrame_;
    }
    s->order = ++gOrder;
    s->msg.priority = prio; s->msg.repetition = rep; s->msg.presentation = pres;
    s->msg.addr0 = p[8];
    char hx[24];
    s->msg.area.clear();
    for (int i = 1; i < al; i++) { snprintf(hx, sizeof hx, "%02X", p[8 + i]); s->msg.area += hx; if (i + 1 < al) s->msg.area += ' '; }
    decodeArea(svc, std::vector<uint8_t>(p + 9, p + 8 + al), s->msg);
    s->msg.sat = p[8] >> 6; s->msg.lesId = p[8] & 63;
    s->msg.lesName = lesName(s->msg.sat, s->msg.lesId);
    bool have = false;
    for (auto& q : s->parts) if (q.first == key) { q.second.assign(p + hdr, p + len - 2); have = true; break; }
    if (!have) s->parts.emplace_back(key, std::vector<uint8_t>(p + hdr, p + len - 2));
    if (!cont) s->lastKey = std::max(s->lastKey, key);
    s->msg.packets = (int)s->parts.size();
    finish(*s);
}

FrameParser::Slot* FrameParser::newSlot() {
    if (slots_.size() >= 150) {      // drop the oldest
        auto it = std::min_element(slots_.begin(), slots_.end(), [](const Slot& a, const Slot& b) { return a.order < b.order; });
        slots_.erase(it);
    }
    slots_.emplace_back();
    return &slots_.back();
}

// A message to a ship (0xAA): station (satellite and LES), logical channel, packet number, then the text. n excludes the check bytes.
// Packets of one logical channel are one message until the station clears the channel (0x27) or starts again at packet 1.
void FrameParser::shipMessage(const uint8_t* p, int n, int64_t rxUnix) {
    if (n < 6) return;
    const int sat = p[2] >> 6, les = p[2] & 63, lcn = p[3], pno = p[4];
    Slot* s = nullptr;
    for (auto& x : slots_)
        if (x.msg.kind == 3 && x.msg.sat == sat && x.msg.lesId == les && x.msg.logicalChannel == lcn && !x.msg.complete) { s = &x; break; }
    if (s && pno == 1 && !s->parts.empty()) {
        bool haveOne = false;
        for (auto& q : s->parts) if (q.first == 2) haveOne = true;
        if (haveOne) { s->msg.complete = true; s->msg.seen++; s = nullptr; }   // a new message on the channel: close the old one
    }
    if (!s) {
        s = newSlot();
        s->msg.kind = 3;
        s->msg.sat = sat; s->msg.lesId = les; s->msg.logicalChannel = lcn;
        s->msg.lesName = lesName(sat, les);
        s->msg.frameNo = curFrame_;
        s->msg.rxUnix = rxUnix; s->msg.rxTime = utcText(rxUnix);
        s->msg.presentation = -1;
        messageCount_++;
        for (auto& an : announcements_)      // the newest announcement of this channel names the ship and the character set
            if (an.sat == sat && an.lesId == les && an.logicalChannel == lcn) { s->msg.mesId = an.mesId; s->msg.presentation = an.presentation; break; }
        char b[96];
        if (s->msg.mesId >= 0) snprintf(b, sizeof b, "Message to ship %d, channel %d", s->msg.mesId, lcn);
        else snprintf(b, sizeof b, "Message to a ship, channel %d", lcn);
        s->msg.serviceText = b;
        s->msg.id = (uint16_t)lcn;
    }
    s->order = ++gOrder;
    const int key = pno * 2;
    bool have = false;
    for (auto& q : s->parts) if (q.first == key) { q.second.assign(p + 5, p + n); have = true; break; }
    if (!have) s->parts.emplace_back(key, std::vector<uint8_t>(p + 5, p + n));
    s->msg.packets = (int)s->parts.size();
    finish(*s);
}

void FrameParser::finish(Slot& s) {
    std::sort(s.parts.begin(), s.parts.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uint8_t> all;
    for (auto& q : s.parts) all.insert(all.end(), q.second.begin(), q.second.end());
    if (s.msg.kind == 3) {       // no last-packet flag: complete when the channel is cleared (parseFrame) or a new message starts
        const int pres = s.msg.presentation >= 0 ? s.msg.presentation : looksBinary(all) ? 7 : 0;
        s.msg.text = decodeText(pres, all);
        return;
    }
    const std::string text = decodeText(s.msg.presentation, all);
    bool done = false;
    if (s.lastKey >= 0) {
        done = true;
        for (int n = 1; n <= s.lastKey / 2; n++) {
            bool g = false;
            for (auto& q : s.parts) if (q.first / 2 == n) g = true;
            if (!g) done = false;
        }
    }
    const bool was = s.msg.complete;
    s.msg.text = text;
    s.msg.complete = done;
    if (done && !was) s.msg.seen++;
}

void FrameParser::fill(InmcTelemetry& t) const {
    t.ncs = ncs_;
    std::copy(pktOk_, pktOk_ + 256, t.pktOk);
    std::copy(pktBad_, pktBad_ + 256, t.pktBad);
    t.packetsOk = packetsOk_; t.packetsBad = packetsBad_;
    t.messageCount = messageCount_;
    t.messages.clear();
    for (const Slot& sl : slots_) {
        t.messages.push_back(sl.msg);
        t.messages.back().order = sl.order;
    }
    trimMessages(t.messages, 150, 70000);
    t.stations = stations_;
    t.announcements = announcements_;
    t.dataValid = false;
    for (auto& s : slots_) if (s.msg.complete) t.dataValid = true;
}

} // namespace inmc
} // namespace dect2
