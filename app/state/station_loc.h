// The one position of this station ("My station"): where the antenna is, its height above sea level and above ground, and where that
// came from. Every screen that needs a position asks here (receiverLocation and friends); ADS-B, the pass planner, the maps, the wizard and
// the My station page all read and write this and nothing else.
//
// Storage keeps today's keys, so a position saved by an earlier version appears unchanged: adsbLat / adsbLon (latitude, longitude), rxAltM
// (the old altitude, read as the fallback) and the new stationAltM, stationAntM, stationLocSource. A saved position always wins over the
// environment (SAURCOM_LOCATOR, or SAURCOM_LAT + SAURCOM_LON), which is only used in memory, with source "env", and never saved.
#pragma once
#include "dect2/geo/station_location.h"
#include <string>

struct App;

// ---- the long-standing calls (about 30 callers) ----
void receiverLocation(double& lat, double& lon);   // the position; a placeholder (25.25, 55.36) until one is set, so maps still open
bool receiverLocationSet();                         // a real position is set (saved, or from the environment)
double receiverAltitudeM();                         // its height above sea level, metres (0 until known)
bool setReceiverLocation(App& a, double lat, double lon, const char* source = "map");   // move it (saved), e.g. Tools > Map's "Use as my location"
void receiverPositionFields(App& a);               // its fields (latitude, longitude, altitude), the paste box and the Locate button (adsb_ui.cpp)

// ---- the station position itself ----
dect2::geo::StationPos stationPos();   // invalid (valid() false) while no position is set
// Validates (latitude / longitude, altitude -500..9000 m, antenna height 0..1000 m), saves, and tells ADS-B. A NaN altitude or antenna height
// keeps the old one (the altitude only when the station moved less than 5 km). False with a one-line reason in *err and nothing changed.
bool setStationPos(App& a, const dect2::geo::StationPos& p, std::string* err = nullptr);
double stationAntennaM();               // antenna height above ground, metres; NaN when not known
std::string stationSource();            // "typed", "pasted", "locator", "map", "gps", "wifi", "env", "legacy"; empty while unset
unsigned stationGeneration();           // changes whenever the position does (screens that cache it compare this)
std::string stationEnvError();          // why SAURCOM_LOCATOR / SAURCOM_LAT were ignored; empty when fine or not set
std::string stationSourceLabel(const std::string& source);   // "typed", "pasted", ... in words, translated

// Ranges and bearings from the station: false while no position is set (show kNoRange then, never a distance from the placeholder)
bool stationRange(double lat, double lon, double& km, double& bearingDeg);
inline constexpr const char* kNoRange = "\xE2\x80\x94";   // the em dash shown for a range while no position is set

// The App the maps save "I'm here" through (adsbmap has no App of its own): bound by stationTick() every loop of the main program.
void stationBindApp(App& a);
App* stationApp();

namespace adsbui { void onStationChanged(App& a); }   // adsb_ui.cpp: take the new position and push it to the decoder
