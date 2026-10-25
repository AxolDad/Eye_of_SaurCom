// The flight recorder: turns the live aircraft table into flights in the store, and keeps running whichever screen the user is looking at.
//
// It sits between the receiver and the two halves below it: the segmenter decides where a flight begins and ends (flights_proto.h), the
// store keeps it (flights_db.h). The program feeds it the telemetry it already polls; nothing here touches the radio or the network.
//
// The identity of an aircraft (registration, type, operator) lives in the program's own registration database, so the recorder asks for it
// through a callback rather than depending on it. Recording can be turned off, and nothing is written while it is.
#pragma once
#include "flights_db.h"
#include "flights_proto.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
struct AdsbTelemetry;
class AirportDb;
}

namespace dect2::flights {

class Recorder {
public:
    Recorder();
    ~Recorder();

    void setStore(Db* db);                       // where finished flights go; nullptr to record nothing
    void setAirports(const AirportDb* db);       // so flights get their airports, runways and approach measurement
    // What the program knows about an aircraft, asked for when a flight is written. Called from the thread that calls feed().
    void setFactsSource(std::function<void(uint32_t icao, AircraftFacts& out)> fn);
    void setReceiver(double lat, double lon);    // for the range figures and for naming the ends
    void clearReceiver();
    void setEnabled(bool on);                    // off: nothing is fed and nothing is written; open flights are dropped
    bool enabled() const;
    void setOptions(const SegmenterOptions& o);  // takes effect for flights that start afterwards

    // One pass of the live aircraft table. `nowWall` is Unix seconds; each aircraft's own age is subtracted from it, so the times stored
    // are when the aircraft was actually heard. Only an aircraft whose message count moved adds anything.
    void feed(const AdsbTelemetry& t, double nowWall);
    // Ends and stores every open flight: when the receiver stops, or at shutdown.
    void flush();
    // Drops every open flight without storing: when the user turns recording off, or clears.
    void discard();

    struct Status {
        bool enabled = true;
        size_t openFlights = 0;
        int64_t flightsWritten = 0;
        int64_t pointsWritten = 0;
        double lastWrite = 0;        // Unix seconds, 0 = nothing yet
        std::string lastFlight;      // what was written last, in words
    };
    Status status() const;

    // The flight being recorded for an aircraft right now, so a screen can draw it before it is finished.
    const Flight* openFlight(uint32_t icao) const;
    const std::vector<Point>* openTrack(uint32_t icao) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The program-wide recorder, driven by the application once a frame.
Recorder& globalRecorder();

} // namespace dect2::flights
