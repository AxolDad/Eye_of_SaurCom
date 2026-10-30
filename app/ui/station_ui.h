// Tools > My station: the pieces more than one screen uses (the paste box with its live preview, used by the page, the setup wizard and the
// ADS-B screen). The window, the toolbar chip and the GPS are in station_ui.cpp (declared in app.h).
#pragma once
#include "../app.h"

// One line under a paste box: "-> 52.10000, -1.30000 (locator IO92)" for text that reads as a location, the reason in red for text that
// does not, nothing for empty text. True (and *out filled) when it reads as a location.
bool stationPreview(const char* text, dect2::geo::StationPos* out);

// A one-line paste box (placeholder "Paste coordinates, a locator (IO91wm) or a Google / OpenStreetMap link") with the live preview and a
// "Use this" button; Enter also applies. Saves through setStationPos. True on the frame the station was set (the text is then cleared).
// `width` is the box's width in pixels (0: the room left).
bool stationPasteBox(App& a, char* buf, size_t n, const char* id, float width = 0);

// Open the My station page (the "Change..." buttons).
void stationOpenPage(App& a);

// While no station position is set: `why` in amber (wrapped) and a "Set your location" button that opens My station. Nothing once one is
// set. For the screens that need the position (ranges, passes, NOTAMs ...).
void stationHint(App& a, const char* why);
