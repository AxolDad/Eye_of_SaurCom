// Maidenhead grid locators (the amateur radio "QTH locator": field, square, subsquare, extended square) and the coordinate text a user
// types: decimal degrees or degrees, minutes, seconds. Pure functions, no state.
#pragma once
#include <string>

namespace dect2 {

// The locator of a position, 2, 4, 6 or 8 characters (e.g. KO24pq34). Latitudes of 90 and longitudes of 180 fall in the last square.
std::string toMaidenhead(double latDeg, double lonDeg, int chars = 6);
// The centre of the square a locator names (2, 4, 6 or 8 characters, either case); false when it is not a locator.
// `heightDeg` / `widthDeg` (or null) get the size of the square.
bool fromMaidenhead(const std::string& loc, double& latDeg, double& lonDeg, double* heightDeg = nullptr, double* widthDeg = nullptr);

// A latitude and a longitude from text: "54.687, 25.280", "54.687N 25.280E", "-33.9 151.2", "54°41'13\"N 25°16'48\"E",
// "54 41 13 N 25 16 48 E", "N54 41.2 E25 16.8", "52.1 -1.3". False when it does not read as a position on Earth; with `checkRange` false the pair is
// returned even when it lies outside +-90 / +-180 (the caller says why), and only text that is not a pair at all gives false.
bool parseCoordinates(const std::string& text, double& latDeg, double& lonDeg, bool checkRange = true);
// True for "52.1 - 1.3" / "10 \xE2\x80\x94 20": a dash with spaces on both sides and no comma before it, which could be a minus sign or a separator.
// parseCoordinates refuses such text.
bool hasAmbiguousDash(const std::string& text);
// One angle as degrees, minutes and seconds with its hemisphere: 54°41'13.2"N
std::string toDms(double deg, bool isLat);

// Distance (km, on a sphere of the mean Earth radius) and initial bearing (degrees from true north) from point 1 to point 2
void greatCircle(double lat1, double lon1, double lat2, double lon2, double& km, double& bearingDeg);

}
