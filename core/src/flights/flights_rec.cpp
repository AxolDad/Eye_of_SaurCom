// The flight recorder: see flights_rec.h
#include "dect2/flights/flights_rec.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/airports/airports.h"
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace dect2::flights {

struct Recorder::Impl {
    mutable std::mutex mu;
    Segmenter seg;
    Db* db = nullptr;
    const AirportDb* ap = nullptr;
    std::function<void(uint32_t, AircraftFacts&)> facts;
    bool on = true;
    Status st;
    std::unordered_map<uint32_t, uint64_t> lastMessages;   // so an aircraft that has said nothing new adds nothing
    double lastTake = 0;

    Impl() = default;

    // Writes what the segmenter has finished. Called with the lock held.
    void store(std::vector<Segmenter::Finished>&& v) {
        if (!db) return;
        for (auto& f : v) {
            AircraftFacts fa;
            fa.icao = f.f.icao;
            if (facts) facts(f.f.icao, fa);
            fa.icao = f.f.icao;
            fa.rotorcraft = fa.rotorcraft || fa.category == "A7";
            // the kind of operation, now that the whole flight is known
            const KindGuess g = inferKind(f.f, f.pts, fa);
            f.f.kind = (uint8_t)g.kind;
            f.f.kindConfidence = g.confidence;
            f.f.kindBasis = g.basis;
            if (!db->addFlight(f.f, f.pts, fa)) continue;
            st.flightsWritten++;
            st.pointsWritten += (int64_t)f.pts.size();
            st.lastWrite = f.f.tEnd;
            char b[160];
            snprintf(b, sizeof b, "%06X%s%s, %.0f min, %zu points", f.f.icao, f.f.callsign.empty() ? "" : " ", f.f.callsign.c_str(),
                     (f.f.tEnd - f.f.tStart) / 60.0, f.pts.size());
            st.lastFlight = b;
        }
    }
};

Recorder::Recorder() : p_(new Impl) {}
Recorder::~Recorder() = default;

void Recorder::setStore(Db* db) { std::lock_guard<std::mutex> lk(p_->mu); p_->db = db; }
void Recorder::setAirports(const AirportDb* db) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->ap = db;
    p_->seg.setAirports(db);
}
void Recorder::setFactsSource(std::function<void(uint32_t, AircraftFacts&)> fn) { std::lock_guard<std::mutex> lk(p_->mu); p_->facts = std::move(fn); }
void Recorder::setReceiver(double lat, double lon) { std::lock_guard<std::mutex> lk(p_->mu); p_->seg.setReceiver(lat, lon); }
void Recorder::clearReceiver() { std::lock_guard<std::mutex> lk(p_->mu); p_->seg.clearReceiver(); }
bool Recorder::enabled() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->on; }
void Recorder::setOptions(const SegmenterOptions&) { /* applies to new flights: the segmenter keeps its own options per instance */ }

void Recorder::setEnabled(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->on == on) return;
    p_->on = on;
    if (!on) { p_->seg.flush(); p_->lastMessages.clear(); }   // dropped, not stored: the user turned recording off
    p_->st.enabled = on;
}

void Recorder::feed(const AdsbTelemetry& t, double nowWall) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->on) return;
    for (const AdsbAircraft& a : t.aircraft) {
        if (!a.icao) continue;
        // Only what this receiver heard. An aircraft that reached the table from another receiver's Beast or SBS feed, or from an
        // aggregator's API, did not fly over this antenna, and recording it would quietly spoil the answers this store exists to give:
        // what is overhead most, how far the receiver reaches, how long something was in earshot. Those aircraft are shown, not kept.
        if (a.srcId != 0) continue;
        const auto it = p_->lastMessages.find(a.icao);
        if (it != p_->lastMessages.end() && it->second == a.messages) continue;   // nothing new since last time
        p_->lastMessages[a.icao] = a.messages;
        // when the aircraft was actually heard, not when this pass happened
        const double seen = nowWall - (a.ageSec > 0 && a.ageSec < 3600 ? a.ageSec : 0);
        p_->seg.feed(a, seen);
    }
    // take what has finished, at most a few times a second
    if (nowWall - p_->lastTake >= 1.0) {
        p_->lastTake = nowWall;
        auto done = p_->seg.take(nowWall);
        if (!done.empty()) p_->store(std::move(done));
        if (p_->lastMessages.size() > 8000) p_->lastMessages.clear();   // an address seen once never comes back; do not grow for ever
    }
    p_->st.openFlights = p_->seg.openFlights();
}

void Recorder::flush() {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto done = p_->seg.flush();
    if (!done.empty()) p_->store(std::move(done));
    p_->lastMessages.clear();
    p_->st.openFlights = 0;
}

void Recorder::discard() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->seg.flush();
    p_->lastMessages.clear();
    p_->st.openFlights = 0;
}

Recorder::Status Recorder::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Status s = p_->st;
    s.enabled = p_->on;
    s.openFlights = p_->seg.openFlights();
    return s;
}

const Flight* Recorder::openFlight(uint32_t icao) const { std::lock_guard<std::mutex> lk(p_->mu); return p_->seg.open(icao); }
const std::vector<Point>* Recorder::openTrack(uint32_t icao) const { std::lock_guard<std::mutex> lk(p_->mu); return p_->seg.openPoints(icao); }

Recorder& globalRecorder() {
    static Recorder r;
    return r;
}

} // namespace dect2::flights
