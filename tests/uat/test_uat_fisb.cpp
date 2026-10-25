// FIS-B off the air: product names, DLAC text, every APDU of a frame, segment reassembly and the pairing of a NOTAM's text with its
// airspace. The frames are from fisb-decode's DO-358B test groups (fisb_frames.h).
#include "dect2/uat/fisb.h"
#include "dect2/uat/uat_proto.h"
#include "dect2/uat/uat_track.h"
#include "fisb_frames.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <random>

using namespace dect2;
#include "../common/test_util.h"

static UatRawFrame frameOf(const char* hex, double t) {
    UatRawFrame raw;
    raw.uplink = true;
    raw.timeSec = t;
    const size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n && i < sizeof raw.data; i++) {
        auto nib = [](char c) { return c >= 'A' ? c - 'A' + 10 : c - '0'; };
        raw.data[i] = (uint8_t)((nib(hex[i * 2]) << 4) | nib(hex[i * 2 + 1]));
    }
    raw.dataBytes = (int)n;
    return raw;
}

static UatDecoded decode(const char* hex, double t) {
    UatDecoded d;
    uatDecodeFrame(frameOf(hex, t), d);
    return d;
}

static const UatFisbProduct* find(const UatTelemetry& tel, uint16_t reportNumber, const std::string& location = "") {
    for (const auto& p : tel.fisb)
        if (p.reportNumber == reportNumber && p.location == location) return &p;
    return nullptr;
}

static void testNames() {
    // These used to be wrong: product 8 was named "AIRMET" and 413 "Winds aloft".
    CHECK(uatFisbProductName(8) == "NOTAM", "8 is %s", uatFisbProductName(8).c_str());
    CHECK(uatFisbProductName(11) == "AIRMET", "11 is %s", uatFisbProductName(11).c_str());
    CHECK(uatFisbProductName(12) == "SIGMET", "12 is %s", uatFisbProductName(12).c_str());
    CHECK(uatFisbProductName(413) != "Winds aloft", "413 is every DLAC text product, not winds aloft");
    CHECK(fisb::productForm(8) == fisb::Form::Twgo && fisb::productForm(413) == fisb::Form::Text, "forms");
}

static void testMetars() {
    // One frame, six METARs: every APDU of a frame is read, not only the first.
    UatDecoded d = decode(kFrameMetar, 10);
    CHECK(d.kind == UatDecoded::Kind::Fisb, "a FIS-B frame");
    CHECK(d.fisbApdus.size() == 6, "six APDUs, got %zu", d.fisbApdus.size());
    CHECK(d.fisb.title == "METAR" && d.fisb.location == "KCEY", "first product %s %s", d.fisb.title.c_str(), d.fisb.location.c_str());
    CHECK(d.fisb.text == "METAR KCEY 300835Z AUTO 29004KT 10SM 04/03 A3014 RMK AO1=", "DLAC text: %s", d.fisb.text.c_str());
    UatTracker tr;
    tr.apply(d, -30, 10);
    tr.apply(decode(kFrameMetar, 20), -30, 20);   // broadcast again: the same six, heard twice
    UatTelemetry tel;
    tr.snapshot(tel, 21);
    CHECK(tel.fisb.size() == 6, "six METARs after a repeat, got %zu", tel.fisb.size());
    bool kgph = false;
    for (const auto& p : tel.fisb) {
        CHECK(p.heard == 2, "%s heard %u times", p.location.c_str(), p.heard);
        kgph = kgph || p.text == "METAR KGPH 300855Z AUTO 00000KT 10SM CLR M01/M02 A3025 RMK AO2=";
    }
    CHECK(kgph, "KGPH read");
    // DO-358B: kept an hour after last heard.
    tr.snapshot(tel, 20 + 3601);
    CHECK(tel.fisb.empty(), "expired after an hour, %zu left", tel.fisb.size());
}

static void testNotamD() {
    UatTracker tr;
    tr.apply(decode(kFrameNotamD, 5), -30, 5);
    UatTelemetry tel;
    tr.snapshot(tel, 6);
    const UatFisbProduct* p = find(tel, 12077, "KFWA");
    CHECK(p, "NOTAM-D 12077 at KFWA");
    if (p) {
        CHECK(p->title == "NOTAM-D" && p->productId == 8 && p->reportYear == 20 && !p->cancelled, "%s y%u", p->title.c_str(), p->reportYear);
        CHECK(p->text.rfind("NOTAM-D KFWA.07/077 240200Z !FWA 07/077 FWA OBST CRANE", 0) == 0, "text: %s", p->text.c_str());
        CHECK(p->text.find("FLAGGED AND LGTD 2007240200-2007240400") != std::string::npos, "the whole text");
    }
    // The same frame carries the graphic of TFR 3298: four records of one report, three distinct circles, one product.
    p = find(tel, 3298);
    CHECK(p && p->title == "NOTAM-TFR" && p->areas.size() == 3, "TFR 3298 graphic, %zu areas", p ? p->areas.size() : 0);
    if (p && p->areas.size() == 3) {
        const auto& a = p->areas[0];
        CHECK(a.ring.empty() && std::fabs(a.radiusNm - 3.0) < 1e-6 && a.floorFt == 0 && a.ceilingFt == 3000 && a.agl, "circle %.1f NM %d-%d agl %d",
              a.radiusNm, a.floorFt, a.ceilingFt, a.agl);
        CHECK(std::fabs(a.centreLat - 39.7169) < 1e-3 && std::fabs(a.centreLon + 86.2949) < 1e-3, "centre %.4f %.4f", a.centreLat, a.centreLon);
    }
}

static void testSegmentedTfr() {
    // NOTAM-TFR 0/6733 is sent in three segments, and its airspace (two circles about Dayton) as a separate graphic in another frame. They
    // must come together as one product, whatever order the frames arrive in.
    UatTracker tr;
    UatTelemetry tel;
    UatDecoded s3 = decode(kFrameTfrSeg3, 1);
    CHECK(s3.fisb.text.empty(), "a segment alone gives no text");
    tr.apply(s3, -30, 1);
    tr.apply(decode(kFrameTfrSeg1, 2), -30, 2);
    tr.apply(decode(kFrameTfrSeg1, 3), -30, 3);   // a repeat of a segment already held
    tr.snapshot(tel, 4);
    CHECK(!find(tel, 6733), "not whole until every segment is in");
    tr.apply(decode(kFrameTfrSeg2, 5), -30, 5);
    tr.snapshot(tel, 6);
    const UatFisbProduct* p = find(tel, 6733);
    CHECK(p && p->title == "NOTAM-TFR", "TFR 6733 put back together");
    if (p) {
        CHECK(p->text.rfind("NOTAM-TFR 0/6733 211945Z PART 1 OF 6 OH..AIRSPACE DAYTON, OHIO", 0) == 0, "text: %.80s", p->text.c_str());
        CHECK(p->text.size() > 900, "all three segments' text, %zu chars", p->text.size());
        CHECK(p->areas.empty(), "no graphic yet");
    }
    tr.apply(decode(kFrameTfrCircle, 7), -30, 7);
    tr.snapshot(tel, 8);
    p = find(tel, 6733);
    size_t n6733 = 0;
    for (const auto& q : tel.fisb) n6733 += q.reportNumber == 6733;
    CHECK(n6733 == 1, "text and graphic are one product, %zu", n6733);
    CHECK(p && !p->text.empty() && p->areas.size() == 2, "text with its two circles, %zu", p ? p->areas.size() : 0);
    if (p && p->areas.size() == 2) {
        for (const auto& a : p->areas) {
            CHECK(std::fabs(a.centreLat - 39.9010) < 1e-3 && std::fabs(a.centreLon + 84.2184) < 1e-3, "centre %.4f %.4f", a.centreLat, a.centreLon);
            CHECK(a.floorFt == 0 && a.ceilingFt == 18000 && !a.agl, "%d-%d MSL", a.floorFt, a.ceilingFt);
        }
        CHECK(std::fabs(p->areas[0].radiusNm - 30) < 1e-6 && std::fabs(p->areas[1].radiusNm - 10) < 1e-6, "30 and 10 NM");
    }
}

static void testRecordStatus() {
    // Two text records in one product, the first in force and the second cancelled: each record's own status counts.
    const uint8_t pl[] = {0x20, 0x20, 0, 0, 0, 0x00,             // text, two records, no location, facility
                          0x00, 0x05, 0x00, 0x04, 0x04,           // 5 bytes, report 1, active
                          0x00, 0x05, 0x00, 0x08, 0x00};          // 5 bytes, report 2, cancelled
    std::vector<UatFisbProduct> v;
    uatFisbProducts(8, pl, sizeof pl, v);
    CHECK(v.size() == 2, "two records, %zu", v.size());
    if (v.size() == 2) {
        CHECK(v[0].reportNumber == 1 && !v[0].cancelled, "first in force");
        CHECK(v[1].reportNumber == 2 && v[1].cancelled, "second cancelled");
    }
    // A record that claims more bytes than there are: nothing, rather than something half read.
    uint8_t bad[sizeof pl];
    memcpy(bad, pl, sizeof pl);
    bad[12] = 0x40;
    v.clear();
    uatFisbProducts(8, bad, sizeof bad, v);
    CHECK(v.empty(), "an overlong record is refused, %zu", v.size());
}

static void testDamage() {
    // Random bytes, and real frames with bytes knocked out: never a crash, never a read past the buffer.
    std::mt19937 rng(978);
    UatTracker tr;
    for (int i = 0; i < 20000; i++) {
        std::vector<uint8_t> b(rng() % 600);
        for (auto& x : b) x = (uint8_t)rng();
        fisb::Apdu a;
        if (fisb::parseApdu(b.data(), b.size(), a)) {
            std::vector<UatFisbProduct> v;
            uatFisbProducts(a.productId, a.payload, a.payloadLen, v);
            uatFisbProducts(8, a.payload, a.payloadLen, v);
            uatFisbProducts(413, a.payload, a.payloadLen, v);
        }
        fisb::dlacDecode(b.data(), b.size());
    }
    const char* frames[] = {kFrameMetar, kFrameNotamD, kFrameTfrCircle, kFrameTfrSeg1, kFrameTfrSeg2, kFrameTfrSeg3};
    for (int i = 0; i < 3000; i++) {
        UatRawFrame raw = frameOf(frames[i % 6], i);
        for (int k = 0; k < 4; k++) raw.data[8 + rng() % (raw.dataBytes - 8)] ^= (uint8_t)(1u << (rng() % 8));
        UatDecoded d;
        uatDecodeFrame(raw, d);
        tr.apply(d, -30, i);
    }
    UatTelemetry tel;
    tr.snapshot(tel, 3000);
    CHECK(tel.fisb.size() <= 2048, "bounded, %zu", tel.fisb.size());
}

int main() {
    testNames();
    testMetars();
    testNotamD();
    testSegmentedTfr();
    testRecordStatus();
    testDamage();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("uat fisb OK\n");
    return 0;
}
