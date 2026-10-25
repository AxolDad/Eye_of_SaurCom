// Airport table: the built-in data, frequency normalisation, search, near, the airband channels of an airport, grouping into captures,
// and the OurAirports CSV import with damaged input.
#include "dect2/airports/airports.h"
#include <cmath>
#include <cstdio>
#include <set>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // ---- frequency normalisation
    CHECK(airportKhz(118.5) == 118500, "118.5");
    CHECK(airportKhz(118.505) == 118505, "8.33 name 118.505");
    CHECK(airportKhz(118.510) == 118510 && airportKhz(118.515) == 118515, "8.33 names");
    CHECK(airportKhz(121.9) == 121900 && airportKhz(121.5) == 121500, "25 kHz");
    CHECK(airportKhz(118.52) == 118525 && airportKhz(118.998) == 119000 && airportKhz(118.502) == 118500, "snap to 25 kHz: %d", airportKhz(118.52));
    CHECK(airportKhz(118.512) == 0 && airportKhz(118.513) == 0, "off raster rejected: %d", airportKhz(118.512));
    CHECK(airportKhz(243.0) == 243000 && airportKhz(123.45) == 123450, "outside the airband kept");
    CHECK(airportKhz(0) == 0 && airportKhz(-5) == 0 && airportKhz(NAN) == 0 && airportKhz(INFINITY) == 0 && airportKhz(9000) == 0, "garbage");
    // the channel naming agrees with the airband mode
    {
        double f; bool n;
        CHECK(airbandParse("118.505", f, n) && n && std::fabs(f - 118.5e6) < 1, "118.505 -> 118.500 8.33");
        CHECK(airbandParse("118.510", f, n) && n && std::fabs(f - 118.508333e6) < 1, "118.510 -> 118.50833");
    }
    // ---- built-in table
    const AirportDb& db = AirportDb::builtin();
    CHECK(db.size() > 10000, "airports %zu", db.size());
    size_t withFreq = 0, nfreq = 0, bad = 0;
    for (const auto& a : db.all()) {
        withFreq += a.airbandCount() > 0;
        for (const auto& f : a.freqs) {
            nfreq++;
            if (f.airband()) { double hz; bool n; char b[24]; snprintf(b, sizeof b, "%d.%03d", f.khz / 1000, f.khz % 1000); if (!airbandParse(b, hz, n) || !airbandOnRaster(hz, n)) bad++; }
        }
    }
    printf("  %zu airports, %zu with airband frequencies, %zu frequencies\n", db.size(), withFreq, nfreq);
    CHECK(withFreq > 9000 && nfreq > 25000 && bad == 0, "airband frequencies off raster: %zu", bad);
    const Airport* lhr = db.find("EGLL");
    CHECK(lhr && lhr == db.find("lhr") && lhr->name.find("Heathrow") != std::string::npos && lhr->country == "GB", "Heathrow by ICAO and IATA");
    CHECK(lhr && std::fabs(lhr->latDeg - 51.47) < 0.05 && std::fabs(lhr->lonDeg + 0.46) < 0.05, "Heathrow position");
    CHECK(lhr && lhr->airbandCount() >= 8, "Heathrow frequencies %zu", lhr ? lhr->airbandCount() : 0);
    CHECK(!db.find("") && !db.find("ZZZZZZ"), "unknown codes");
    // ---- search
    {
        auto h = db.search("heathrow");
        CHECK(!h.empty() && db.all()[h[0].index].icao == "EGLL", "heathrow first");
        h = db.search("EGLL");
        CHECK(h.size() >= 1 && db.all()[h[0].index].icao == "EGLL", "ICAO");
        h = db.search("lhr");
        CHECK(h.size() >= 1 && db.all()[h[0].index].icao == "EGLL", "IATA");
        h = db.search("london");
        bool gatwick = false, heath = false;
        for (const auto& x : h) { gatwick |= db.all()[x.index].icao == "EGKK"; heath |= db.all()[x.index].icao == "EGLL"; }
        CHECK(gatwick && heath, "london finds Gatwick and Heathrow (%zu hits)", h.size());
        h = db.search("vilnius");
        CHECK(!h.empty() && db.all()[h[0].index].icao == "EYVI", "Vilnius: %s", h.empty() ? "-" : db.all()[h[0].index].icao.c_str());
        h = db.search("  VILNIUS  int ");
        CHECK(!h.empty() && db.all()[h[0].index].icao == "EYVI", "several words, any case");
        CHECK(db.search("").empty() && db.search("   ").empty() && db.search("qqqqzzzz").empty(), "nothing");
        const double me[2] = {54.9, 23.9};   // Kaunas: nearer than the other Lithuanian airports with the same name word
        h = db.search("kaunas", 5, me);
        CHECK(!h.empty() && h[0].km >= 0 && h[0].km < 30, "Kaunas from Kaunas");
        h = db.search("a", 10);
        CHECK(h.size() == 10, "short query is capped");
        h = db.search("paris", 100, nullptr, true);
        for (const auto& x : h) CHECK(db.all()[x.index].airbandCount() > 0, "withFreqOnly");
    }
    // ---- near
    {
        const auto n = db.nearby(54.64, 25.28, 60);
        CHECK(!n.empty() && db.all()[n[0].index].icao == "EYVI" && n[0].km < 10, "nearest to Vilnius");
        for (size_t i = 1; i < n.size(); i++) CHECK(n[i].km >= n[i - 1].km && n[i].km <= 60, "sorted and inside");
        CHECK(db.nearby(54.64, 25.28, 0.1, 5, true).size() <= 1, "tiny radius");
        CHECK(db.nearby(-60, 0, 10).empty(), "the sea");
        CHECK(db.nearby(54.64, 25.28, 5000, 7).size() == 7, "capped");
        CHECK(std::fabs(airportDistanceKm(0, 0, 0, 1) - 111.19) < 0.1 && airportDistanceKm(10, 10, 10, 10) == 0, "distance");
    }
    // ---- the channels of an airport, and their grouping
    {
        int skipped = 0;
        const auto ch = airportAirbandChannels(*lhr, &skipped);
        CHECK(ch.size() >= 8 && ch.size() <= lhr->airbandCount(), "channels %zu of %zu", ch.size(), lhr->airbandCount());
        CHECK(skipped >= 1, "Heathrow lists VOR-ATIS frequencies outside the airband: %d", skipped);
        std::set<long> seen;
        std::set<std::string> labels;
        bool sorted = true;
        for (size_t i = 0; i < ch.size(); i++) {
            CHECK(ch[i].label.compare(0, 5, "EGLL ") == 0, "label %s", ch[i].label.c_str());
            CHECK(airbandOnRaster(ch[i].freqHz, ch[i].is833), "on raster %.1f", ch[i].freqHz);
            CHECK(seen.insert(std::lround(ch[i].freqHz)).second && labels.insert(ch[i].label).second, "duplicate %s", ch[i].label.c_str());
            if (i && ch[i].freqHz < ch[i - 1].freqHz) sorted = false;
        }
        CHECK(sorted, "sorted by frequency");
        bool tower = false;
        for (const auto& c : ch) tower |= c.label.find("Tower") != std::string::npos;
        CHECK(tower, "a Tower channel");
        std::vector<double> f;
        for (const auto& c : ch) f.push_back(c.freqHz);
        const double span = airbandUsableSpanHz(2.4e6);
        CHECK(span > 1.5e6 && span < 2.2e6, "usable span at 2.4 Msps: %.0f", span);
        CHECK(airbandUsableSpanHz(8e6) > 7e6 && airbandUsableSpanHz(100e3) == 0, "usable span scales");
        const auto g = airbandPlanGroups(f, span);
        size_t total = 0;
        for (const auto& x : g) { total += x.members.size(); CHECK(x.hiHz - x.loHz <= span + 1, "group fits"); }
        CHECK(total == f.size() && !g.empty(), "all channels in a group");
        const auto g8 = airbandPlanGroups(f, airbandUsableSpanHz(20e6));
        CHECK(g8.size() == 1, "one group at 20 Msps: %zu", g8.size());
        // a small airport: the 8.33 kHz channels keep their flags
        Airport t;
        t.icao = "XXXX";
        t.freqs = {{"TWR", "", 118505}, {"GND", "", 121900}, {"CTAF", "", 121900}, {"ATIS", "", 118510}, {"TWR", "", 119700}, {"TWR", "second", 120700}, {"MISC", "", 243000}};
        int sk = 0;
        const auto c2 = airportAirbandChannels(t, &sk);
        CHECK(c2.size() == 5 && sk == 1, "synthetic: %zu channels, %d skipped", c2.size(), sk);
        CHECK(c2.size() == 5 && c2[0].is833 && c2[1].is833 && !c2[2].is833 && !c2[4].is833 && std::fabs(c2[0].freqHz - 118.5e6) < 1 && std::fabs(c2[1].freqHz - 118.508333e6) < 1, "8.33 flags");
        bool both = false;
        for (const auto& c : c2) both |= c.label == "XXXX Ground/CTAF";
        CHECK(both, "one frequency, two roles");
        CHECK(c2.size() == 5 && c2[0].label == "XXXX Tower 1" && c2[2].label == "XXXX Tower 2" && c2[3].label == "XXXX Tower 3", "numbered");
        const Airport none;
        CHECK(airportAirbandChannels(none).empty(), "no frequencies");
    }
    // ---- text round trip and damaged text
    {
        AirportDb d;
        const auto r = d.parseTsv(db.toTsv());
        CHECK(r.airports == db.size() && r.freqs == nfreq && r.skipped == 0, "round trip: %zu/%zu, %zu skipped", r.airports, db.size(), r.skipped);
        AirportDb e;
        const auto r2 = e.parseTsv("# c\nA\tAAAA\tAAA\tName\tCity\tXX\t10.0\t20.0\t5\tM\t1\nF\tTWR\t118500\tdesc\nF\tbad\n"
                                   "A\tBBBB\t\tNo position\tC\tXX\tnan\t20\t0\tS\t0\nF\tTWR\t121500\t\nF\tTWR\t999999999\t\nX\tnonsense\n\n"
                                   "A\tCCCC\n"
                                   "A\tDDDD\t\tD\tDC\tXX\t95\t20\t0\tS\t0\nA\tEEEE\t\tE\tEC\tXX\t-10.5\t179.9\t-20\tS\t0\nF\tATIS\t118505\r\n");
        CHECK(e.size() == 2 && r2.airports == 2 && r2.freqs == 2 && r2.skipped >= 6, "damaged: %zu airports, %zu freqs, %zu skipped", r2.airports, r2.freqs, r2.skipped);
        CHECK(e.size() == 2 && e.all()[0].freqs.size() == 1 && e.all()[1].freqs.size() == 1 && e.all()[1].elevFt == -20, "frequencies stay with their airport");
        AirportDb z;
        CHECK(z.parseTsv("").airports == 0 && z.empty() && z.search("x").empty() && z.nearby(0, 0, 10).empty(), "empty table");
    }
    // ---- OurAirports CSV import
    {
        const std::string ap =
            "\"id\",\"ident\",\"type\",\"name\",\"latitude_deg\",\"longitude_deg\",\"elevation_ft\",\"continent\",\"iso_country\",\"iso_region\",\"municipality\",\"scheduled_service\",\"icao_code\",\"iata_code\",\"gps_code\",\"local_code\",\"home_link\",\"wikipedia_link\",\"keywords\"\r\n"
            "1,\"EGXX\",\"large_airport\",\"Test \"\"Field\"\", North\",51.5,-0.5,83,\"EU\",\"GB\",\"GB-ENG\",\"London, Greater\",\"yes\",\"EGXX\",\"XXX\",\"EGXX\",,,\r\n"
            "2,\"00AA\",\"small_airport\",\"Ranch\",38.7,-101.4,3435,\"NA\",\"US\",\"US-KS\",\"Leoti\",\"no\",,,\"00AA\",\"00AA\",,,\n"
            "3,\"00AB\",\"small_airport\",\"Strip with radio\",39.0,-100.0,100,\"NA\",\"US\",\"US-KS\",\"X\",\"no\",,,\"00AB\",\"00AB\",,,\n"
            "4,\"00H1\",\"heliport\",\"Pad\",40,-70,11,\"NA\",\"US\",\"US-PA\",\"Y\",\"no\",,,\"00H1\",\"00H1\",,,\n"
            "5,\"00CL\",\"closed\",\"Gone\",40,-70,11,\"NA\",\"US\",\"US-PA\",\"Y\",\"no\",,,\"00CL\",\"00CL\",,,\n"
            "6,\"BADP\",\"medium_airport\",\"No position\",,,11,\"NA\",\"US\",\"US-PA\",\"Y\",\"no\",,,\"BADP\",\"BADP\",,,\n"
            "7,\"EYSA\",\"medium_airport\",\"Siauliai\",55.9,23.4,420,\"EU\",\"LT\",\"LT-SA\",\"Siauliai\",\"no\",\"EYSA\",\"SQQ\",\"EYSA\",,,,\n"
            "short\n";
        const std::string fr =
            "\"id\",\"airport_ref\",\"airport_ident\",\"type\",\"description\",\"frequency_mhz\"\n"
            "10,1,\"EGXX\",\"TWR\",\"Test Tower\",118.505\n"
            "11,1,\"EGXX\",\"ATIS\",\"ATIS\",113.75\n"
            "12,1,\"EGXX\",\"GND\",,121.9\n"
            "13,1,\"EGXX\",\"GND\",,not a number\n"
            "14,1,\"EGXX\",\"UNIC\",\"Unicom\",118.512\n"
            "15,3,\"00AB\",\"CTAF\",\"CTAF\",122.9\n"
            "16,99,\"NONE\",\"TWR\",,118.0\n"
            "17\n";
        AirportDb d;
        const auto r = d.importOurAirports(ap, fr);
        CHECK(d.size() == 3 && r.airports == 3, "imported %zu airports", d.size());
        const Airport* a = d.find("EGXX");
        CHECK(a && a->name == "Test \"Field\", North" && a->city == "London, Greater" && a->scheduled && a->kind == 'L' && a->iata == "XXX", "quoted fields");
        CHECK(a && a->freqs.size() == 3, "frequencies of EGXX: %zu (bad number and off-raster dropped)", a ? a->freqs.size() : 0);
        CHECK(a && a->freqs.size() == 3 && a->freqs[0].type == "ATIS" && a->freqs[0].khz == 113750 && a->freqs[2].khz == 118505 && a->freqs[2].desc == "Test Tower", "values");
        CHECK(d.find("00AB") && d.find("00AB")->freqs.size() == 1 && d.find("00AB")->freqs[0].desc.empty(), "airport without a code uses ident; description equal to type dropped");
        CHECK(!d.find("00AA"), "a small airport without service or frequency is left out");
        CHECK(!d.find("00H1") && !d.find("00CL") && !d.find("BADP"), "heliport, closed and positionless left out");
        CHECK(d.find("SQQ") && d.find("SQQ")->icao == "EYSA", "IATA lookup of an airport with no frequencies (medium)");
        AirportDb e;
        CHECK(e.importOurAirports("garbage,garbage\nx,y\n", "").airports == 0 && e.empty(), "not an OurAirports file");
        CHECK(e.importOurAirports("", "").airports == 0, "empty files");
        // the exported text of an import loads again unchanged
        AirportDb f;
        const auto r3 = f.parseTsv(d.toTsv());
        CHECK(r3.airports == 3 && r3.freqs == r.freqs, "import -> text -> load");
    }
    // ---- runways
    {
        const AirportDb& db = AirportDb::builtin();
        size_t rw = 0, withRw = 0;
        for (const auto& a : db.all()) { rw += a.runways.size(); withRw += !a.runways.empty(); }
        printf("  %zu runways at %zu airports\n", rw, withRw);
        CHECK(rw > 9000 && withRw > 6000, "built-in runways: %zu at %zu airports", rw, withRw);
        // KJFK: four runways, the longest 14511 ft (13R/31L), all paved and lit
        const Airport* jfk = db.find("KJFK");
        CHECK(jfk && jfk->runways.size() == 4, "KJFK runways: %zu", jfk ? jfk->runways.size() : 0);
        CHECK(jfk && jfk->longestRunwayFt() == 14511, "KJFK longest %d ft", jfk ? jfk->longestRunwayFt() : 0);
        if (jfk) for (const auto& r : jfk->runways) CHECK(r.hard && r.lit && r.le.hasPos && r.he.hasPos && r.le.hasHeading, "KJFK %s/%s", r.le.ident.c_str(), r.he.ident.c_str());
        // 04L / 22R: headings about 31 and 211, and the two ends roughly opposite
        const Runway* r04 = nullptr;
        if (jfk) for (const auto& r : jfk->runways) if (r.le.ident == "04L") r04 = &r;
        CHECK(r04 && r04->he.ident == "22R", "04L pairs with %s", r04 ? r04->he.ident.c_str() : "?");
        CHECK(r04 && std::fabs(r04->le.headingDegT - 31) < 2 && std::fabs(r04->he.headingDegT - 211) < 2, "04L/22R headings %.1f %.1f",
              r04 ? r04->le.headingDegT : -1, r04 ? r04->he.headingDegT : -1);
        CHECK(r04 && r04->lengthFt == 12079 && r04->le.hasElev && r04->le.elevFt == 12, "04L %d ft, elev %d", r04 ? r04->lengthFt : 0, r04 ? r04->le.elevFt : -1);
        // which end an arriving aircraft is using, from its track
        if (r04) {
            const RunwayEnd* e = r04->endForTrack(30);
            CHECK(e && e->ident == "04L", "track 030 -> %s", e ? e->ident.c_str() : "(none)");
            e = r04->endForTrack(215);
            CHECK(e && e->ident == "22R", "track 215 -> %s", e ? e->ident.c_str() : "(none)");
            CHECK(!r04->endForTrack(121), "a track across the runway matches an end");
            e = r04->endForTrack(359);   // wraps past north
            CHECK(e && e->ident == "04L", "track 359 -> %s", e ? e->ident.c_str() : "(none)");
        }
        // the nearest airport with a long runway, for naming where a flight ended
        const Airport* n = db.nearestWithRunway(40.64, -73.78, 20, 10000);
        CHECK(n && n->icao == "KJFK", "nearest 10000 ft runway to JFK: %s", n ? n->icao.c_str() : "(none)");
        CHECK(!db.nearestWithRunway(40.64, -73.78, 20, 20000), "a runway longer than any");
        CHECK(!db.nearestWithRunway(0, 0, 50, 0), "middle of the Atlantic");
    }
    // ---- runways: the text format and the OurAirports CSV
    {
        AirportDb d;
        d.parseTsv("A\tEGXX\tXXX\tTest\tLondon\tGB\t51.5\t-0.5\t80\tL\t1\n"
                   "A\tEGYY\t\tOther\tLeeds\tGB\t53.8\t-1.5\t300\tM\t0\n");
        const auto r = d.parseRunwaysTsv(
            "# a comment\n"
            "R\tEGXX\t09\t51.5\t-0.51\t80\t90\t27\t51.5\t-0.49\t80\t270\t9000\t150\t1\t1\n"
            "R\tEGXX\tH1\t51.501\t-0.5\t\t\t\t\t\t\t\t60\t60\t1\t0\n"      // a helipad: one end, no heading
            "R\tZZZZ\t18\t1\t2\t0\t180\t36\t1.1\t2\t0\t0\t5000\t100\t1\t1\n"   // an airport not in the table
            "R\tEGYY\t05\t\t\t\t\t23\t\t\t\t\t4000\t75\t0\t0\n"                // neither end has a position
            "R\tEGXX\tshort\n");
        CHECK(r.runways == 2 && r.skipped == 3, "parsed %zu runways, %zu skipped", r.runways, r.skipped);
        const Airport* x = d.find("EGXX");
        CHECK(x && x->runways.size() == 2 && x->longestRunwayFt() == 9000, "EGXX %zu runways", x ? x->runways.size() : 0);
        CHECK(x && x->runways[0].le.hasHeading && x->runways[0].le.headingDegT == 90 && x->runways[0].hard && x->runways[0].lit, "09 values");
        CHECK(x && !x->runways[1].le.hasHeading && !x->runways[1].le.hasElev && x->runways[1].le.hasPos, "the helipad: position only");
        CHECK(x && !x->runways[1].endForTrack(90), "an end without a heading is never chosen");
        CHECK(d.find("EGYY") && d.find("EGYY")->runways.empty(), "EGYY has none");
        // the exported text loads again unchanged
        AirportDb e;
        e.parseTsv(d.toTsv());
        const auto r2 = e.parseRunwaysTsv(d.runwaysToTsv());
        CHECK(r2.runways == 2 && r2.skipped == 0, "runways -> text -> load: %zu, %zu skipped", r2.runways, r2.skipped);
        const Airport* x2 = e.find("EGXX");
        CHECK(x2 && x2->runways.size() == 2 && x2->runways[0].le.headingDegT == 90 && x2->runways[0].le.elevFt == 80, "values survive the round trip");
        CHECK(x2 && !x2->runways[1].le.hasHeading && !x2->runways[1].le.hasElev, "the unknowns stay unknown");
        // the OurAirports CSV, with a closed runway and a heading it does not give
        const auto r3 = d.importRunways(
            "\"id\",\"airport_ref\",\"airport_ident\",\"length_ft\",\"width_ft\",\"surface\",\"lighted\",\"closed\","
            "\"le_ident\",\"le_latitude_deg\",\"le_longitude_deg\",\"le_elevation_ft\",\"le_heading_degT\","
            "\"he_ident\",\"he_latitude_deg\",\"he_longitude_deg\",\"he_elevation_ft\",\"he_heading_degT\"\n"
            "1,1,\"EGXX\",9000,150,\"ASPH\",1,0,\"09\",51.5,-0.51,80,,\"27\",51.5,-0.49,80,\n"
            "2,1,\"EGXX\",3000,60,\"GRASS\",0,0,\"18\",51.51,-0.5,80,180,\"36\",51.49,-0.5,80,360\n"
            "3,1,\"EGXX\",2000,50,\"ASPH\",1,1,\"05\",51.5,-0.5,80,50,\"23\",51.5,-0.5,80,230\n"
            "4,2,\"ZZZZ\",5000,100,\"CONC\",1,0,\"01\",1,2,0,10,\"19\",1.1,2,0,190\n");
        CHECK(r3.runways == 2 && r3.skipped == 2, "CSV: %zu runways, %zu skipped (closed and unknown airport)", r3.runways, r3.skipped);
        const Airport* x3 = d.find("EGXX");
        CHECK(x3 && x3->runways.size() == 2, "EGXX from CSV: %zu", x3 ? x3->runways.size() : 0);
        // the missing heading of 09/27 computed from the thresholds: due east and due west
        CHECK(x3 && x3->runways[0].le.hasHeading && std::fabs(x3->runways[0].le.headingDegT - 90) < 1, "computed heading %.1f", x3 ? x3->runways[0].le.headingDegT : -1);
        CHECK(x3 && std::fabs(x3->runways[0].he.headingDegT - 270) < 1, "computed reciprocal %.1f", x3 ? x3->runways[0].he.headingDegT : -1);
        CHECK(x3 && x3->runways[0].hard && !x3->runways[1].hard, "surfaces: asphalt hard, grass not");
        AirportDb f;
        CHECK(f.importRunways("nonsense\n1,2\n").runways == 0, "not an OurAirports runways file");
    }
    return testSummary("airports");
}
