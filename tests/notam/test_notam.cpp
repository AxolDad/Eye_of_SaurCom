// NOTAMs: the readers for tfr.faa.gov (against files it served), the FAA NOTAM API (against its documented geoJson layout: no key was
// at hand to record a real answer) and FIS-B, and the merge that falls back from the most accurate source to the next.
#include "dect2/notam/notam.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

using namespace dect2;
using namespace dect2::notam;
#include "../common/test_util.h"

static std::string readFile(const std::string& name) {
    std::ifstream f(std::string(SAURCOM_NOTAM_TESTDATA) + "/" + name, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static const Notam* byKey(const std::vector<Notam>& v, const std::string& k) {
    for (const Notam& n : v)
        if (n.key == k) return &n;
    return nullptr;
}

static void testTimes() {
    CHECK(parseIsoUtc("2026-10-26T13:00:00") == 1793019600, "plain");
    CHECK(parseIsoUtc("2026-10-26T13:00:00.000Z") == 1793019600, "with Z");
    CHECK(parseIsoUtc("2026-10-26T09:00:00-04:00") == 1793019600, "with an offset");
    CHECK(parseIsoUtc("garbage") == 0 && parseIsoUtc("") == 0 && parseIsoUtc("2026-13-01T00:00") == 0, "unreadable is 0");
    CHECK(parseNotamTime("2007240200") == 1595556000, "YYMMDDHHMM");
    CHECK(parseNotamTime("20072402") == 0, "short");
}

static void testTfrSite() {
    Snapshot s;
    std::string err;
    CHECK(parseTfrList(readFile("tfr_list.json"), 1000, s, &err), "list: %s", err.c_str());
    CHECK(s.items.size() == 79 && s.complete && s.kinds.size() == 1 && s.kinds[0] == "TFR", "79 TFRs, got %zu", s.items.size());
    const Notam* n = byKey(s.items, "FDC 6/2203");
    CHECK(n && n->type == "HAZARDS" && n->location == "ZTL" && n->text.empty(), "6/2203 from the list");
    CHECK(tfrListVersion(readFile("tfr_list.json"), "6/2203") == "202610251909", "its version");
    CHECK(tfrDetailUrl("6/2203") == "https://tfr.faa.gov/download/detail_6_2203.xml", "detail URL %s", tfrDetailUrl("6/2203").c_str());
    CHECK(tfrDetailUrl("6/0100") == "https://tfr.faa.gov/download/detail_6_0100.xml", "leading zeros kept");
    CHECK(tfrDetailUrl("../x") == "" && tfrDetailUrl("6/22a3") == "", "only a NOTAM number makes a URL");

    Notam d = *n;
    CHECK(parseTfrDetail(readFile("detail_6_2203.xml"), d, &err), "detail: %s", err.c_str());
    CHECK(d.startUtc == 1793019600 && d.endUtc == 1793055600, "UTC times %lld %lld", (long long)d.startUtc, (long long)d.endUtc);
    CHECK(d.text.rfind("!FDC 6/2203 ZTL GA..AIRSPACE 9NM NE GRAY, GA", 0) == 0, "text %.60s", d.text.c_str());
    CHECK(d.areas.size() == 1 && d.areas[0].ring.size() >= 30, "one outline");
    if (!d.areas.empty()) {
        const Area& a = d.areas[0];
        CHECK(a.hasLimits && a.floorFt == 0 && a.ceilingFt == 1500 && !a.floorAgl && !a.ceilingAgl, "SFC-1500 MSL: %d-%d", a.floorFt, a.ceilingFt);
        CHECK(std::fabs(a.ring[0].first - 33.19173051) < 1e-6 && std::fabs(a.ring[0].second + 83.46666667) < 1e-6, "N and W read");
        CHECK(distanceNm(d, 33.158, -83.47) == 0, "its centre is inside");
        const double far = distanceNm(d, 33.158 + 1, -83.47);
        CHECK(far > 50 && far < 60, "a degree north is about 55 NM out: %.1f", far);
    }
    // Local times: EDT is UTC-4.
    Notam e;
    CHECK(parseTfrDetail(readFile("detail_6_2131.xml"), e, &err), "detail 2131: %s", err.c_str());
    CHECK(e.key == "FDC 6/2131" && e.startUtc == 1793125800, "14:30 EDT is 18:30Z: %lld", (long long)e.startUtc);
    CHECK(e.areas.size() == 2, "two areas, %zu", e.areas.size());
    for (const Area& a : e.areas) CHECK(a.hasLimits && a.ceilingFt == 4999 && a.ceilingAgl, "up to 4999 AGL");
    // A detail for another NOTAM than the one asked for is refused.
    Notam other = *byKey(s.items, "FDC 6/2204");
    CHECK(!parseTfrDetail(readFile("detail_6_2203.xml"), other), "mismatched detail refused");
}

static void testFaaApi() {
    // The layout of the API's geoJson answer, as documented: items[].properties.coreNOTAMData.notam, .notamTranslation, and .geometry.
    const std::string body = R"({"pageSize":1000,"pageNum":1,"totalCount":3,"totalPages":1,"items":[
      {"type":"Feature","properties":{"coreNOTAMData":{"notamEvent":{"scenario":"6000"},"notam":{"id":"NOTAM_1","number":"6/2203","type":"N",
        "classification":"FDC","location":"ZTL","accountId":"FDC","effectiveStart":"2026-10-26T13:00:00.000Z","effectiveEnd":"2026-10-26T23:00:00.000Z",
        "text":"ZTL GA..AIRSPACE 9NM NE GRAY, GA..TEMPORARY FLIGHT RESTRICTION.","radius":"3","lowerLimit":"SFC","upperLimit":"1500FT"},
        "notamTranslation":[{"type":"LOCAL_FORMAT","simpleText":"!FDC 6/2203 ZTL GA..AIRSPACE 9NM NE GRAY, GA..TEMPORARY FLIGHT RESTRICTION."}]}},
       "geometry":{"type":"GeometryCollection","geometries":[{"type":"Point","coordinates":[-83.45,33.16]}]}},
      {"type":"Feature","properties":{"coreNOTAMData":{"notam":{"number":"07/077","type":"N","classification":"DOM","location":"FWA",
        "icaoLocation":"KFWA","accountId":"FWA","effectiveStart":"2026-10-20T00:00:00.000Z","effectiveEnd":"PERM","text":"OBST CRANE"}}},
       "geometry":{"type":"Polygon","coordinates":[[[-85.2,40.9],[-85.1,40.9],[-85.1,41.0],[-85.2,40.9]]]}},
      {"type":"Feature","properties":{"coreNOTAMData":{"notam":{"number":"07/078","type":"C","classification":"DOM","accountId":"FWA","text":"CANCELLED"}}}}
    ]})";
    std::vector<Notam> v;
    int pages = 0;
    std::string err;
    CHECK(parseFaaApi(body, 2000, v, pages, &err) && pages == 1, "read: %s", err.c_str());
    CHECK(v.size() == 3, "three, %zu", v.size());
    const Notam* t = byKey(v, "FDC 6/2203");
    CHECK(t && t->kind == "TFR" && t->startUtc == 1793019600 && t->endUtc == 1793055600, "the TFR");
    CHECK(t && t->text.rfind("!FDC 6/2203", 0) == 0, "local format text preferred");
    CHECK(t && t->areas.size() == 1 && t->areas[0].radiusNm == 3 && t->areas[0].ceilingFt == 1500 && t->areas[0].floorAgl, "a 3 NM circle SFC-1500");
    const Notam* d = byKey(v, "D FWA 7/77");
    CHECK(d && d->kind == "D" && d->permanent && d->location == "KFWA" && d->areas.size() == 1 && d->areas[0].ring.size() == 4, "the NOTAM-D");
    if (d && !d->areas.empty()) CHECK(std::fabs(d->areas[0].ring[0].first - 40.9) < 1e-9, "lat from GeoJSON's second coordinate");
    const Notam* c = byKey(v, "D FWA 7/78");
    CHECK(c && c->cancelled, "type C is a cancellation");
    CHECK(!parseFaaApi("{\"error\":\"unauthorized\"}", 0, v, pages), "an error answer is no answer");

    CHECK(faaApiUrl(33.1, -83.4, 250, 2).find("locationRadius=100&") != std::string::npos, "radius clamped to the API's 100 NM");
    CHECK(faaApiUrl(33.1, -83.4, 25, 2).find("pageNum=2") != std::string::npos, "page");
    CHECK(faaApiUrl(NAN, 0, 25, 1).empty(), "no position, no query");
    CHECK(faaKeyValid("a1b2c3d4e5") && !faaKeyValid("a b") && !faaKeyValid("x'y") && !faaKeyValid("x\ny") && !faaKeyValid(""), "key check");
}

static void testNms() {
    CHECK(nmsTokenUrl(NmsEnv::Production) == "https://api-nms.aim.faa.gov/v1/auth/token", "token URL %s", nmsTokenUrl(NmsEnv::Production).c_str());
    CHECK(nmsTokenUrl(NmsEnv::Staging) == "https://api-staging.cgifederal-aim.com/v1/auth/token", "staging token URL");
    const std::string u = nmsNotamsUrl(NmsEnv::Production, 33.1, -83.4, 250);
    CHECK(u == "https://api-nms.aim.faa.gov/nmsapi/v1/notams?latitude=33.1000&longitude=-83.4000&radius=100", "notams URL %s", u.c_str());
    CHECK(nmsNotamsUrl(NmsEnv::Fit, NAN, 0, 10).empty(), "no position, no query");
    CHECK(nmsBasicAuthHeader("abc", "xyz") == "Authorization: Basic YWJjOnh5eg==", "%s", nmsBasicAuthHeader("abc", "xyz").c_str());
    CHECK(nmsBasicAuthHeader("ab", "c") == "Authorization: Basic YWI6Yw==", "padding: %s", nmsBasicAuthHeader("ab", "c").c_str());

    std::string tok, err;
    double exp = 0;
    CHECK(parseNmsToken(R"({"access_token":"eyJ0.abc-_","token_type":"Bearer","expires_in":1799})", tok, exp, &err) && tok == "eyJ0.abc-_" && exp == 1799,
          "token: %s", err.c_str());
    CHECK(parseNmsToken(R"({"access_token":"t","expires_in":"300"})", tok, exp) && exp == 300, "expires_in as a string");
    CHECK(!parseNmsToken(R"({"error":"invalid_client"})", tok, exp) && tok.empty(), "no token");
    CHECK(!parseNmsToken(R"({"access_token":"a b"})", tok, exp), "a token that cannot go in a header");

    const std::string body = R"({"status":"Success","data":{"geojson":[
      {"type":"Feature","properties":{"coreNOTAMData":{"notam":{"number":"07/077","classification":"DOMESTIC","accountId":"FWA",
        "icaoLocation":"KFWA","effectiveStart":"2026-10-20T00:00:00.000Z","effectiveEnd":"2026-11-20T00:00:00.000Z","text":"OBST CRANE"}}},
       "geometry":{"type":"Point","coordinates":[-85.2,40.9]}},
      {"type":"Feature","properties":{"coreNOTAMData":{"notam":{"number":"6/2203","classification":"FDC",
        "text":"ZTL GA..TEMPORARY FLIGHT RESTRICTION."}}}}]}})";
    std::vector<Notam> v;
    CHECK(parseNms(body, 3000, v, &err) && v.size() == 2, "read: %s, %zu", err.c_str(), v.size());
    const Notam* d = byKey(v, "D FWA 7/77");
    CHECK(d && d->kind == "D" && d->endUtc > 0, "DOMESTIC is a NOTAM-D");
    CHECK(byKey(v, "FDC 6/2203") && byKey(v, "FDC 6/2203")->kind == "TFR", "an FDC TFR");
    v.clear();
    CHECK(!parseNms(R"({"timestamp":"x","status":"401","message":"Unauthorized"})", 0, v, &err) && err == "NMS: Unauthorized", "error: %s", err.c_str());

    // NMS is the most accurate: its text over the older API's.
    Snapshot nms; nms.source = Source::Nms; nms.ok = true; nms.asOfUtc = 9990;
    Snapshot old; old.source = Source::FaaApi; old.ok = true; old.asOfUtc = 9990;
    Notam a; a.key = "FDC 6/1"; a.kind = "TFR"; a.text = "NMS"; a.seenUtc = 9990;
    Notam b = a; b.text = "OLD API";
    nms.items = {a};
    old.items = {b};
    auto m = merge({old, nms}, 10000);
    CHECK(m.size() == 1 && m[0].text == "NMS" && m[0].textFrom == Source::Nms, "NMS first");
    // NMS out of reach for an hour: the older API's live answer is used.
    nms.ok = false;
    nms.asOfUtc = 6000;
    m = merge({old, nms}, 10000);
    CHECK(m.size() == 1 && m[0].text == "OLD API" && !m[0].stale, "falls back to the older API");
}

static UatFisbProduct fisbProduct(const std::string& title, const std::string& text, uint16_t rn, uint8_t ry, const std::string& loc, double t) {
    UatFisbProduct p;
    p.productId = 8;
    p.title = title;
    p.text = text;
    p.reportNumber = rn;
    p.reportYear = ry;
    p.location = loc;
    p.timeSec = t;
    return p;
}

static void testFisb() {
    std::vector<UatFisbProduct> ps;
    ps.push_back(fisbProduct("NOTAM-D", "NOTAM-D KFWA.07/077 240200Z !FWA 07/077 FWA OBST CRANE (ASN UNKNOWN) 875FT (60FT AGL) FLAGGED AND LGTD 2007240200-2007240400",
                             12077, 20, "KFWA", 50));
    ps.push_back(fisbProduct("NOTAM-TFR", "NOTAM-TFR 6/2203 251909Z ZTL GA..AIRSPACE 9NM NE GRAY", 2203, 6, "", 60));
    UatFisbProduct g = fisbProduct("NOTAM-TFR", "", 2204, 6, "", 70);   // a graphic before its text
    UatFisbProduct::Area a;
    a.centreLat = 33.5; a.centreLon = -112.2; a.radiusNm = 3; a.floorFt = 0; a.ceilingFt = 400; a.agl = true;
    g.areas.push_back(a);
    ps.push_back(g);
    UatFisbProduct metar = fisbProduct("METAR", "METAR KCEY 300835Z AUTO", 0, 0, "KCEY", 80);
    metar.productId = 413;
    ps.push_back(metar);
    Snapshot s = fromFisb(ps, 1000, 1080);
    CHECK(s.items.size() == 3 && !s.complete, "three NOTAMs, not the METAR: %zu", s.items.size());
    const Notam* d = byKey(s.items, "D FWA 7/77");
    CHECK(d && d->startUtc == 1595556000 && d->endUtc == 1595563200 && d->seenUtc == 1050, "NOTAM-D keyed by accountability, its times");
    CHECK(byKey(s.items, "FDC 6/2203"), "TFR keyed by its number");
    const Notam* gr = byKey(s.items, "FDC 6/2204");
    CHECK(gr && gr->areas.size() == 1 && gr->areas[0].ceilingAgl, "a graphic alone keyed by its report");
}

static Notam item(const std::string& key, double seen, const std::string& text = "", const std::string& kind = "TFR") {
    Notam n;
    n.key = key;
    n.kind = kind;
    n.text = text;
    n.seenUtc = seen;
    return n;
}

static Area circle(double lat, double lon, double r) {
    Area a;
    a.centreLat = lat; a.centreLon = lon; a.radiusNm = r;
    return a;
}

static void testMerge() {
    const double now = 10000;
    Snapshot site; site.source = Source::TfrSite; site.ok = true; site.asOfUtc = now - 60; site.complete = true; site.kinds = {"TFR"};
    Snapshot fisb; fisb.source = Source::Fisb; fisb.ok = true; fisb.asOfUtc = now - 5;

    // Both have it: the site's text and outline, FIS-B noted as a second source.
    Notam a = item("FDC 6/1", now - 60, "SITE TEXT");
    Area ring; ring.ring = {{33, -84}, {33.1, -84}, {33.1, -83.9}};
    a.areas = {ring};
    a.startUtc = 9000; a.endUtc = 20000;
    site.items.push_back(a);
    Notam af = item("FDC 6/1", now - 5, "FISB TEXT");
    af.areas = {circle(33.05, -83.95, 3)};
    fisb.items.push_back(af);
    // Only FIS-B has it, heard after the site was fetched: a new TFR the site has not got yet. Kept.
    fisb.items.push_back(item("FDC 6/2", now - 5, "NEW"));
    // FIS-B last heard it before the site was fetched, and the site does not list it: gone.
    fisb.items.push_back(item("FDC 6/3", now - 120, "OLD"));
    // Over.
    Notam over = item("FDC 6/4", now - 60, "OVER");
    over.endUtc = (int64_t)now - 1;
    site.items.push_back(over);
    // A NOTAM-D: the site does not vouch for those, so its absence there says nothing.
    fisb.items.push_back(item("D FWA 7/77", now - 600, "CRANE", "D"));

    auto m = merge({site, fisb}, now);
    const Notam* n1 = byKey(m, "FDC 6/1");
    CHECK(n1 && n1->text == "SITE TEXT" && n1->textFrom == Source::TfrSite && n1->areasFrom == Source::TfrSite && n1->areas[0].ring.size() == 3,
          "site first for text and outline");
    CHECK(n1 && n1->sources == ((1 << (int)Source::TfrSite) | (1 << (int)Source::Fisb)) && !n1->stale && !n1->partStale, "both sources, live");
    CHECK(n1 && n1->startUtc == 9000 && n1->timesFrom == Source::TfrSite, "times from the site");
    CHECK(byKey(m, "FDC 6/2"), "a TFR newer than the site's list is kept");
    CHECK(!byKey(m, "FDC 6/3"), "a TFR the site no longer lists is gone");
    CHECK(!byKey(m, "FDC 6/4"), "an ended TFR is gone");
    CHECK(byKey(m, "D FWA 7/77"), "a NOTAM-D is not the site's to drop");

    // The site goes stale (its fetches fail): FIS-B's live text and circle win, and the site's TFRs no live source has are shown as stale.
    Snapshot old = site;
    old.ok = false;
    old.asOfUtc = now - 7200;
    for (Notam& x : old.items) x.seenUtc = now - 7200;
    old.items.push_back(item("FDC 6/5", now - 7200, "ONLY ON THE SITE"));
    m = merge({old, fisb}, now);
    n1 = byKey(m, "FDC 6/1");
    CHECK(n1 && n1->text == "FISB TEXT" && n1->areasFrom == Source::Fisb && n1->areas[0].radiusNm == 3, "live FIS-B over a stale site");
    CHECK(n1 && n1->timesFrom == Source::TfrSite && n1->partStale && !n1->stale, "times only the stale site has: shown, and flagged");
    const Notam* n5 = byKey(m, "FDC 6/5");
    CHECK(n5 && n5->stale && std::fabs(n5->ageSec - 7200) < 1e-6, "stale, with its age");
    CHECK(byKey(m, "FDC 6/3"), "a stale source cannot say something is gone");

    // The newest word is a cancellation: gone.
    Snapshot api; api.source = Source::FaaApi; api.ok = true; api.asOfUtc = now - 1;
    Notam c = item("FDC 6/2", now - 1, "", "TFR");
    c.cancelled = true;
    api.items.push_back(c);
    m = merge({site, fisb, api}, now);
    CHECK(!byKey(m, "FDC 6/2"), "cancelled by the newest source");

    // A regional source vouches only for its circle: absent and inside is gone, absent and far away says nothing.
    Snapshot reg; reg.source = Source::FaaApi; reg.ok = true; reg.asOfUtc = now - 1; reg.complete = true;
    reg.hasRegion = true; reg.regionLat = 33; reg.regionLon = -84; reg.regionNm = 50;
    Snapshot f2; f2.source = Source::Fisb; f2.ok = true; f2.asOfUtc = now - 5;
    Notam in = item("FDC 6/7", now - 100, "IN");
    in.areas = {circle(33.1, -84.1, 3)};
    Notam out = item("FDC 6/8", now - 100, "OUT");
    out.areas = {circle(40, -100, 3)};
    f2.items = {in, out, item("FDC 6/9", now - 100, "NOWHERE")};
    m = merge({reg, f2}, now);
    CHECK(!byKey(m, "FDC 6/7") && byKey(m, "FDC 6/8") && byKey(m, "FDC 6/9"), "only what is inside the circle is the API's to drop");
}

static void testDamage() {
    std::mt19937 rng(7);
    const std::string xml = readFile("detail_6_2203.xml"), list = readFile("tfr_list.json");
    for (int i = 0; i < 300; i++) {
        std::string x = xml, l = list;
        for (int k = 0; k < 20; k++) {
            x[rng() % x.size()] = (char)rng();
            l[rng() % l.size()] = (char)rng();
        }
        x.resize(rng() % x.size());
        Notam n;
        parseTfrDetail(x, n);
        Snapshot s;
        parseTfrList(l, 0, s);
        std::vector<Notam> v;
        int p;
        parseFaaApi(l, 0, v, p);
        distanceNm(n, 33, -84);
    }
    CHECK(true, "no crash");
}

int main() {
    testTimes();
    testTfrSite();
    testFaaApi();
    testNms();
    testFisb();
    testMerge();
    testDamage();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("notam OK\n");
    return 0;
}
