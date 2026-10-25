// Asking an aggregator's web API what is in the air (core/include/dect2/adsb/adsb_api.h): the settings, the polling thread, and the section
// of the Data feeds window that drives it.
//
// Off until the user turns a service on. What arrives is shown on the map and in the table as a source of its own; it is never passed on to
// the outbound feeds and never recorded in the flight history, for the reasons set out in adsb_api.h.
#pragma once

struct App;

namespace adsbapi {

// Once a frame, cheap: starts and stops the polling thread, and hands what has arrived to the receiver. Call it whichever window is open.
void tick(App& a);
// The "Bring aircraft in" section of the Data feeds window.
void panel(App& a);
// At exit: stop the thread before the receiver goes away.
void shutdown();

}  // namespace adsbapi
