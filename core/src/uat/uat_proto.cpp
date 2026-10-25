// UAT protocol layer: wraps dump978 uat_decode and maps into UatTelemetry types.
#include "dect2/uat/uat_proto.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/uat/uat_decode_c.h"
#include "dect2/uat/uat_frame.h"
#include "dect2/uat/fisb.h"
#include <algorithm>
#include <map>
#include <cstdio>
#include <cstring>
#include "dect2/common/str.h"

namespace dect2 {

namespace {

std::string hexOf(const uint8_t* d, int n) { return dect2::str::toHex(d, n > 0 ? (size_t)n : 0); }

void fillAcFromMdb(const uat_adsb_mdb& m, UatAircraft& ac) {
    ac.icao = m.address & 0xFFFFFF;
    if (m.callsign_type == CS_CALLSIGN && m.callsign[0]) ac.callsign = m.callsign;
    if (m.callsign_type == CS_SQUAWK) { ac.hasSquawk = true; ac.squawk = atoi(m.callsign); }
    if (m.position_valid) { ac.hasPos = true; ac.lat = m.lat; ac.lon = m.lon; }
    if (m.altitude_type == ALT_BARO) { ac.hasAlt = true; ac.altFt = m.altitude; }
    if (m.altitude_type == ALT_GEO) { ac.hasGeoAlt = true; ac.geoAltFt = m.altitude; }
    if (m.speed_valid) { ac.hasSpeed = true; ac.speedKt = m.speed; }
    if (m.track_type != TT_INVALID) { ac.hasHeading = true; ac.headingDeg = (float)m.track; }
    if (m.vert_rate) { ac.hasVrate = true; ac.vrateFpm = m.vert_rate; }
    ac.ground = m.airground_state == AG_GROUND;
    ac.nic = m.nic;
    ac.tisb = m.address_qualifier == AQ_TISB_ICAO || m.address_qualifier == AQ_TISB_OTHER;
}

// The kind of a DLAC text product, from its own first word; the station from its second.
void textKind(const std::string& t, std::string& kind, std::string& station) {
    kind.clear();
    station.clear();
    size_t a = t.find_first_of(" \n");
    const std::string first = t.substr(0, a);
    static const char* kKinds[] = {"METAR", "SPECI", "TAF", "TAF.AMD", "PIREP", "WINDS"};
    for (const char* k : kKinds)
        if (first == k) kind = k;
    if (kind.empty() || a == std::string::npos) return;
    while (a < t.size() && (t[a] == ' ' || t[a] == '\n')) a++;
    const size_t b = t.find_first_of(" \n", a);
    station = t.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

// The kind of NOTAM a TWGO text record holds, from its own first word ("NOTAM-D", "NOTAM-FDC", "NOTAM-TFR" ...).
std::string notamKind(const std::string& t) {
    const size_t a = t.find(' ');
    const std::string first = t.substr(0, a);
    return first.rfind("NOTAM", 0) == 0 ? first : std::string();
}

// The airspace of one graphic record. A polygon often comes as the same ring twice, one copy at the floor and one at the ceiling; a ring
// with a single altitude reaches from the surface up to it. A circle is a prism with its own floor and ceiling. Lines and points are not
// airspace and are left out.
void areasOf(const fisb::GraphicRecord& g, std::vector<UatFisbProduct::Area>& out) {
    const bool agl = g.altitudesAgl();
    for (const fisb::Prism& z : g.prisms) {
        UatFisbProduct::Area a;
        a.centreLat = z.latBottom;
        a.centreLon = z.lonBottom;
        a.radiusNm = z.rMajorNm;
        a.floorFt = z.bottomFt;
        a.ceilingFt = z.topFt;
        a.agl = agl;
        out.push_back(std::move(a));
    }
    if ((g.geometry == 3 || g.geometry == 4) && g.vertices.size() >= 3) {
        std::map<int, std::vector<std::pair<double, double>>> byAlt;
        for (const fisb::Vertex& v : g.vertices) byAlt[v.altFt].push_back({v.lat, v.lon});
        UatFisbProduct::Area a;
        a.ring = byAlt.begin()->second;
        a.floorFt = byAlt.size() >= 2 ? byAlt.begin()->first : 0;
        a.ceilingFt = byAlt.rbegin()->first;
        a.agl = agl;
        if (a.ring.size() >= 3) out.push_back(std::move(a));
    }
}

} // namespace

// The names come from fisb.h, which follows DO-358B (and dump978 for the older numbering). This used to name product 8 "AIRMET" (it is
// NOTAMs, TFRs included) and 413 "Winds aloft" (it is every DLAC text product), among others.
std::string uatFisbProductName(uint16_t productId) {
    const char* n = fisb::productName(productId);
    if (n && *n) return n;
    char b[32];
    snprintf(b, sizeof b, "FIS-B %u", productId);
    return b;
}

void uatFisbProducts(uint16_t productId, const uint8_t* payload, size_t n, std::vector<UatFisbProduct>& out) {
    if (!payload || n == 0) return;
    const fisb::Form form = fisb::productForm(productId);
    if (form == fisb::Form::Text) {
        UatFisbProduct p;
        p.productId = productId;
        p.text = fisb::dlacDecode(payload, n);
        if (p.text.empty()) return;
        std::string kind, station;
        textKind(p.text, kind, station);
        p.title = kind.empty() ? uatFisbProductName(productId) : kind;
        p.location = station;
        out.push_back(std::move(p));
        return;
    }
    if (form != fisb::Form::Twgo) return;   // gridded images: not text, and nothing here draws them yet
    fisb::Twgo tw;
    if (!fisb::decodeTwgo(payload, n, productId, tw)) return;
    // DO-358B: a reference point other than a facility (0x00) or external (0xFF) means the records are to be ignored.
    if (tw.referencePoint != 0x00 && tw.referencePoint != 0xFF) return;
    for (const fisb::TextRecord& r : tw.text) {
        UatFisbProduct p;
        p.productId = productId;
        p.location = tw.location;
        p.reportNumber = r.reportNumber;
        p.reportYear = r.reportYear;
        p.cancelled = !r.active;
        p.text = r.text;
        const std::string k = notamKind(r.text);
        p.title = k.empty() ? uatFisbProductName(productId) : k;
        out.push_back(std::move(p));
    }
    // One report's airspace often comes as several records in one APDU (an area for each period it applies, or each part of the shape):
    // they make one product, with each distinct area once.
    const size_t firstGraphic = out.size();
    for (const fisb::GraphicRecord& g : tw.graphics) {
        UatFisbProduct* p = nullptr;
        for (size_t i = firstGraphic; i < out.size(); i++)
            if (out[i].reportNumber == g.reportNumber && out[i].reportYear == g.reportYear) p = &out[i];
        if (!p) {
            out.emplace_back();
            p = &out.back();
            p->productId = productId;
            p->location = tw.location;
            p->reportNumber = g.reportNumber;
            p->reportYear = g.reportYear;
            // A NOTAM's graphic says it is a TFR (object element 0) before its text has come.
            p->title = productId == 8 && g.objectElement == 0 && tw.location.empty() ? "NOTAM-TFR" : uatFisbProductName(productId);
        }
        if (g.cancelled()) p->cancelled = true;
        std::vector<UatFisbProduct::Area> v;
        areasOf(g, v);
        for (UatFisbProduct::Area& a : v) {
            bool dup = false;
            for (const UatFisbProduct::Area& b : p->areas)
                dup = dup || (a.ring == b.ring && a.centreLat == b.centreLat && a.centreLon == b.centreLon && a.radiusNm == b.radiusNm &&
                              a.floorFt == b.floorFt && a.ceilingFt == b.ceilingFt && a.agl == b.agl);
            if (!dup) p->areas.push_back(std::move(a));
        }
    }
}

bool uatDecodeFrame(const UatRawFrame& raw, UatDecoded& out) {
    out = UatDecoded();
    out.hex = hexOf(raw.data, raw.dataBytes);
    uint8_t frame[UAT_UPLINK_FRAME_BYTES];
    std::memcpy(frame, raw.data, (size_t)raw.dataBytes);
    if (raw.uplink) {
        uat_uplink_mdb mdb;
        memset(&mdb, 0, sizeof mdb);
        uat_decode_uplink_mdb(frame, &mdb);
        if (!mdb.app_data_valid) {
            out.kind = UatDecoded::Kind::Other;
            out.summary = "UAT uplink (no application data)";
            return true;
        }
        // Every FIS-B APDU of the frame: a frame usually carries several, and this used to stop at the first.
        std::map<std::string, int> names;
        bool haveFirst = false;
        for (unsigned i = 0; i < mdb.num_info_frames; i++) {
            const auto& fr = mdb.info_frames[i];
            if (!fr.is_fisb || !fr.data || fr.length == 0) continue;
            out.kind = UatDecoded::Kind::Fisb;
            out.fisbApdus.emplace_back(fr.data, fr.data + fr.length);
            fisb::Apdu a;
            if (!fisb::parseApdu(fr.data, fr.length, a)) continue;
            names[uatFisbProductName(a.productId)]++;
            if (haveFirst || a.segmented) continue;   // a segment means nothing alone: the tracker puts the product back together
            std::vector<UatFisbProduct> v;
            uatFisbProducts(a.productId, a.payload, a.payloadLen, v);
            for (const UatFisbProduct& p : v)
                if (!p.text.empty()) { out.fisb = p; out.fisb.timeSec = raw.timeSec; haveFirst = true; break; }
        }
        if (out.kind == UatDecoded::Kind::Fisb) {
            out.summary = "FIS-B";
            for (const auto& kv : names) out.summary += " " + kv.first + (kv.second > 1 ? " x" + std::to_string(kv.second) : "");
            if (haveFirst) out.summary += ": " + out.fisb.text.substr(0, 120);
            return true;
        }
        out.kind = UatDecoded::Kind::Other;
        out.summary = "UAT uplink application data";
        return true;
    }

    const bool shortFrame = (frame[0] >> 3) == 0;
    const int mdbType = (frame[0] >> 3) & 0x1f;
    if (mdbType == 0 || mdbType == 1) {
        uat_adsb_mdb mdb;
        memset(&mdb, 0, sizeof mdb);
        uat_decode_adsb_mdb(frame, &mdb);
        fillAcFromMdb(mdb, out.ac);
        out.kind = out.ac.tisb ? UatDecoded::Kind::Tisb : UatDecoded::Kind::Adsb;
        char b[160];
        if (out.ac.hasPos && out.ac.hasAlt)
            snprintf(b, sizeof b, "%s %06X %.4f %.4f %d ft", out.ac.tisb ? "TIS-B" : "UAT ADS-B", out.ac.icao, out.ac.lat, out.ac.lon, out.ac.altFt);
        else if (!out.ac.callsign.empty())
            snprintf(b, sizeof b, "%s %06X %s", out.ac.tisb ? "TIS-B" : "UAT ADS-B", out.ac.icao, out.ac.callsign.c_str());
        else
            snprintf(b, sizeof b, "%s %06X type %d", out.ac.tisb ? "TIS-B" : "UAT ADS-B", out.ac.icao, mdbType);
        out.summary = b;
        return true;
    }

    out.kind = UatDecoded::Kind::Other;
    char b[80];
    snprintf(b, sizeof b, "UAT MDB type %d (%s frame)", mdbType, shortFrame ? "short" : "long");
    out.summary = b;
    return true;
}

} // namespace dect2
