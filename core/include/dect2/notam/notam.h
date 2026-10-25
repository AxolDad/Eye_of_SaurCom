// NOTAMs and TFRs from every source this program can reach, merged so that what is shown is the most accurate information available and
// never something out of date passed off as current.
//
// The sources, most accurate first:
//   FaaApi   the FAA NOTAM API (external-api.faa.gov): every NOTAM, with times and geometry. Needs a free key from api.faa.gov.
//   TfrSite  tfr.faa.gov: every TFR in the country, its airspace drawn exactly and its times. No key.
//   Fisb     FIS-B, off the air on 978 MHz: the NOTAMs and TFRs the ground stations near you broadcast. No internet at all.
//
// How they are merged ("fail backwards"):
//   - one NOTAM is one key ("FDC 6/2203", "D FWA 7/77") whichever source it came from;
//   - each field comes from the most accurate source that is live and has it: the text from the API before the TFR site before FIS-B, the
//     airspace from the TFR site (which draws it exactly) before the API before FIS-B. A field only a stale source has is still shown, and
//     says so;
//   - a source is live while its last good fetch (or frame) is recent enough; a NOTAM no live source has is shown as stale, with its age;
//   - a NOTAM a complete, live source does not list, though that source looked after every other source last had it, is gone. "Complete"
//     is what the source vouches for: the TFR site for every TFR, the API for everything within the circle it was asked about, FIS-B for
//     nothing (a ground station sends what is near it, and what it skips says nothing);
//   - a NOTAM whose end has passed, or that its newest source says was cancelled, is not shown.
//
// Everything here is pure: readers for what the sources send, and the merge. The fetching lives in the application.
#pragma once
#include "dect2/uat/uat_tel.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::notam {

enum class Source : uint8_t { FaaApi = 0, TfrSite = 1, Fisb = 2 };
constexpr int kSources = 3;
const char* sourceName(Source s);

// One piece of airspace: a polygon, or a circle when the ring is empty.
struct Area {
    std::vector<std::pair<double, double>> ring;   // lat, lon
    double centreLat = 0, centreLon = 0, radiusNm = 0;
    bool hasLimits = false;
    int floorFt = 0, ceilingFt = 0;
    bool floorAgl = false, ceilingAgl = false;     // above ground rather than above sea level
};

struct Notam {
    std::string key;          // what names it across sources: "FDC 6/2203" (FDC NOTAMs and TFRs), "D FWA 7/77" (NOTAM-D, by accountability)
    std::string number;       // as issued: "6/2203", "07/077"
    std::string kind;         // "TFR", "FDC", "D", "TRA", "TMOA", or "" when the source does not say
    std::string location;     // the airport or facility
    std::string type;         // a TFR's kind: SECURITY, VIP, HAZARDS, SPACE OPERATIONS ...
    std::string title;        // one line to list it by
    std::string text;
    int64_t startUtc = 0, endUtc = 0;   // unix seconds; 0 when not known
    bool permanent = false;
    bool cancelled = false;
    std::vector<Area> areas;
    double seenUtc = 0;       // when its source last had it

    // Filled in by merge(): where each part came from, and whether any of it is out of date.
    Source textFrom = Source::Fisb, areasFrom = Source::Fisb, timesFrom = Source::Fisb;
    uint8_t sources = 0;      // a bit (1 << Source) for every source that has it
    bool stale = false;       // no live source has it: shown from the last one that did
    bool partStale = false;   // live, but a field (the airspace, the times) is from a source that is not
    double ageSec = 0;        // since the freshest source had it
};

// What one source said, the last time it was asked.
struct Snapshot {
    Source source = Source::Fisb;
    bool ok = false;          // false: never fetched, or the last fetch failed; its items are what it had before
    double asOfUtc = 0;       // when it was last fetched (or, for FIS-B, last heard)
    bool complete = false;    // true: what it does not list (of the kinds and region below) does not exist
    std::vector<std::string> kinds;   // the kinds it is complete for; empty for every kind
    bool hasRegion = false;
    double regionLat = 0, regionLon = 0, regionNm = 0;
    std::vector<Notam> items;
};

struct MergeOptions {
    double liveSec[kSources] = {1800, 1800, 900};   // a source is live this long after its last good fetch (FIS-B: last frame)
};

// The NOTAMs to show, sorted by start time. nowUtc is the wall clock, in the same seconds as asOfUtc and seenUtc.
std::vector<Notam> merge(const std::vector<Snapshot>& snaps, double nowUtc, const MergeOptions& opt = MergeOptions());

// ---------------------------------------------------------------- tfr.faa.gov

// https://tfr.faa.gov/tfrapi/getTfrList: every TFR in force or coming, without its airspace. Fills a complete TFR snapshot.
bool parseTfrList(const std::string& body, double asOfUtc, Snapshot& out, std::string* err = nullptr);
// What the list says changes a TFR: fetch its detail again when this differs ("202610252014").
std::string tfrListVersion(const std::string& body, const std::string& number);
// https://tfr.faa.gov/download/detail_6_2203.xml: one TFR whole (XNOTAM-Update). Fills the text, the times and the airspace into `n`,
// keeping what the list gave where the detail says nothing.
bool parseTfrDetail(const std::string& xml, Notam& n, std::string* err = nullptr);
std::string tfrDetailUrl(const std::string& number);   // "6/2203" -> the detail's URL; "" for a number that is not one

// ---------------------------------------------------------------- the FAA NOTAM API

// The query for everything within radiusNm (the API allows 100) of a point, one page of up to 1000.
std::string faaApiUrl(double lat, double lon, double radiusNm, int page);
// Reads one page (geoJson). totalPages is how many there are.
bool parseFaaApi(const std::string& body, double asOfUtc, std::vector<Notam>& out, int& totalPages, std::string* err = nullptr);
// True when the id and secret can go in a request header as they are: printable, no spaces, quotes or line breaks.
bool faaKeyValid(const std::string& s);

// ---------------------------------------------------------------- FIS-B

// The NOTAM products a UAT receiver has heard (product 8, 16, 17), as one snapshot. signalToUtc turns a product's time into the wall clock.
Snapshot fromFisb(const std::vector<UatFisbProduct>& products, double signalToUtc, double asOfUtc);

// ---------------------------------------------------------------- helpers

// Unix seconds from "2026-10-26T13:00:00", "2026-10-26T13:00:00.000Z" or "2026-10-26T13:00:00-04:00"; 0 when unreadable.
int64_t parseIsoUtc(const std::string& s);
// Unix seconds from a NOTAM's "2007240200" (YYMMDDHHMM, UTC); 0 when unreadable.
int64_t parseNotamTime(const std::string& s);
// How far a point is from a NOTAM's airspace: 0 inside, nautical miles outside, -1 when it has no airspace.
double distanceNm(const Notam& n, double lat, double lon);

} // namespace dect2::notam
