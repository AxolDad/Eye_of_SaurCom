// NOTAMs and TFRs (core/include/dect2/notam/notam.h) in the program: the fetching, FIS-B from any UAT receiver, the merge, the NOTAMs
// window, and the TFR layer on the ADS-B map.
//
// FIS-B needs nothing: whatever a UAT receiver hears is used. tfr.faa.gov and the FAA NOTAM API are off until the user turns them on; the
// API also needs the user's own free key. The last good answer of each is kept in the data folder, so a start without internet still shows
// what was known, marked with its age.
#pragma once
#include "dect2/notam/notam.h"
#include <string>
#include <vector>

struct App;
struct ImDrawList;

namespace notamapp {

void tick(App& a);        // once a frame, cheap: FIS-B in, fetches started, the merge redone every few seconds
void window(App& a);      // Tools > NOTAMs and TFRs (a.notamOpen)
void shutdown();          // at exit: the fetch thread stops
// The ADS-B map: the airspace of every NOTAM that has some, coloured by whether it is in force now. The map's projection must be current.
void drawMap(ImDrawList* dl);
bool mapLayerOn();
void setMapLayer(bool on);
// The NOTAMs for an airport ("KFWA" also finds a NOTAM-D of accountability FWA), and any whose airspace comes within nearNm of it: for the
// airport window. Pass nearNm 0 to match by the identifier alone.
std::vector<dect2::notam::Notam> forAirport(const std::string& ident, double lat, double lon, double nearNm);

}  // namespace notamapp
