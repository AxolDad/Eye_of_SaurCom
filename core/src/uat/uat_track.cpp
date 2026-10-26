#include "dect2/uat/uat_track.h"
#include "dect2/adsb/adsb_msg.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

void UatTracker::reset() {
    ac_.clear(); fisb_.clear(); unseg_.clear(); log_.clear();
    good_ = bad_ = 0; goodWindow_ = 0; windowT0_ = 0;
}

void UatTracker::setReference(double lat, double lon) { refValid_ = true; refLat_ = lat; refLon_ = lon; }
void UatTracker::clearReference() { refValid_ = false; }

void UatTracker::apply(const UatDecoded& d, float levelDbfs, double timeSec) {
    good_++;
    if (windowT0_ <= 0) windowT0_ = timeSec;
    if (timeSec - windowT0_ <= 1.0) goodWindow_++;

    UatFrameInfo fi;
    fi.hex = d.hex;
    fi.timeSec = timeSec;
    fi.levelDbfs = levelDbfs;
    fi.summary = d.summary;
    switch (d.kind) {
    case UatDecoded::Kind::Adsb: fi.kind = "ADS-B"; break;
    case UatDecoded::Kind::Tisb: fi.kind = "TIS-B"; break;
    case UatDecoded::Kind::Fisb: fi.kind = "FIS-B"; break;
    default: fi.kind = "UAT"; break;
    }

    if (d.kind == UatDecoded::Kind::Fisb) {
        for (const auto& apdu : d.fisbApdus) {
            fisb::Apdu a;
            if (!fisb::parseApdu(apdu.data(), apdu.size(), a)) continue;
            std::vector<UatFisbProduct> v;
            if (a.segmented) {
                std::vector<uint8_t> whole;
                if (!unseg_.add(a, timeSec, whole)) continue;
                uatFisbProducts(a.productId, whole.data(), whole.size(), v);
            } else {
                uatFisbProducts(a.productId, a.payload, a.payloadLen, v);
            }
            for (UatFisbProduct& p : v) addFisb(std::move(p), a.month, timeSec);
        }
        unseg_.expire(timeSec, 600);
        log_.push_back(fi);
        while (log_.size() > 64) log_.pop_front();
        return;
    }

    if (d.kind == UatDecoded::Kind::Adsb || d.kind == UatDecoded::Kind::Tisb) {
        fi.address = d.ac.icao;
        Rec& r = ac_[d.ac.icao];
        UatAircraft& ac = r.ac;
        if (!ac.icao) ac.icao = d.ac.icao;
        if (!d.ac.callsign.empty()) ac.callsign = d.ac.callsign;
        if (d.ac.hasSquawk) { ac.hasSquawk = true; ac.squawk = d.ac.squawk; }
        if (d.ac.hasAlt) { ac.hasAlt = true; ac.altFt = d.ac.altFt; }
        if (d.ac.hasGeoAlt) { ac.hasGeoAlt = true; ac.geoAltFt = d.ac.geoAltFt; }
        if (d.ac.hasPos) {
            ac.hasPos = true; ac.lat = d.ac.lat; ac.lon = d.ac.lon;
            ac.track.push_back({(float)d.ac.lat, (float)d.ac.lon});
            while (ac.track.size() > 64) ac.track.erase(ac.track.begin());
        }
        if (d.ac.hasSpeed) { ac.hasSpeed = true; ac.speedKt = d.ac.speedKt; }
        if (d.ac.hasHeading) { ac.hasHeading = true; ac.headingDeg = d.ac.headingDeg; }
        if (d.ac.hasVrate) { ac.hasVrate = true; ac.vrateFpm = d.ac.vrateFpm; }
        ac.ground = d.ac.ground;
        ac.tisb = d.ac.tisb;
        if (d.ac.nic >= 0) ac.nic = d.ac.nic;
        ac.messages++;
        ac.levelDbfs = levelDbfs;
        ac.ageSec = 0;
        r.last = timeSec;
        if (refValid_ && ac.hasPos) {
            ac.hasRange = true;
            ac.distNm = (float)adsb::distanceNm(refLat_, refLon_, ac.lat, ac.lon);
            ac.bearingDeg = (float)adsb::bearingDeg(refLat_, refLon_, ac.lat, ac.lon);
        }
    }

    log_.push_back(fi);
    while (log_.size() > 64) log_.pop_front();
}

// DO-358B B.3.3 names a TWGO report by its product, year, number, location and the month of the APDU; its text and its graphic come
// as separate records under that one name and are put together here. A DLAC text product has no number: a METAR or a TAF is one per
// station and kind, the newest replacing the last; a PIREP is one per text.
void UatTracker::addFisb(UatFisbProduct p, int month, double timeSec) {
    std::string key;
    if (fisb::productForm(p.productId) == fisb::Form::Twgo)
        key = std::to_string(p.productId) + "-" + std::to_string(p.reportYear) + "-" + std::to_string(p.reportNumber) + "-" +
              p.location + "-" + std::to_string(month);
    else if (!p.location.empty() && p.title != "PIREP")
        key = std::to_string(p.productId) + "-" + p.title + "-" + p.location;
    else
        key = std::to_string(p.productId) + "-" + p.text;
    auto it = fisb_.find(key);
    if (it == fisb_.end()) {
        p.timeSec = p.firstSec = timeSec;
        p.heard = 1;
        // A graphic that comes before its text: keep it, and give it a title until the text says what it is.
        fisb_.emplace(key, std::move(p));
    } else {
        UatFisbProduct& q = it->second;
        q.timeSec = timeSec;
        q.heard++;
        if (p.cancelled) q.cancelled = true;
        if (!p.text.empty()) {
            // Text that changed is a new version of the report: what was drawn for the old one may no longer agree with it.
            if (!q.text.empty() && q.text != p.text) q.areas.clear();
            q.text = std::move(p.text);
            q.title = std::move(p.title);
        }
        if (!p.areas.empty()) q.areas = std::move(p.areas);
    }
    // Bounded, whatever is on the air: the stalest go first.
    while (fisb_.size() > 2048) {
        auto oldest = fisb_.begin();
        for (auto j = fisb_.begin(); j != fisb_.end(); ++j)
            if (j->second.timeSec < oldest->second.timeSec) oldest = j;
        fisb_.erase(oldest);
    }
}

void UatTracker::tick(double now) {
    for (auto it = ac_.begin(); it != ac_.end();) {
        it->second.ac.ageSec = (float)(now - it->second.last);
        if (now - it->second.last > expirySec_) it = ac_.erase(it);
        else ++it;
    }
    if (now - windowT0_ > 1.0) { goodWindow_ = 0; windowT0_ = now; }
    // DO-358B keeps a report for an hour after it was last heard: ground stations repeat what is current far more often than that.
    for (auto it = fisb_.begin(); it != fisb_.end();) {
        if (now - it->second.timeSec > 3600) it = fisb_.erase(it);
        else ++it;
    }
}

void UatTracker::snapshot(UatTelemetry& out, double now) {
    tick(now);
    out.aircraft.clear();
    out.fisb.clear();
    for (const auto& kv : fisb_)
        if (!kv.second.text.empty() || !kv.second.areas.empty() || kv.second.cancelled) out.fisb.push_back(kv.second);
    std::sort(out.fisb.begin(), out.fisb.end(), [](const UatFisbProduct& a, const UatFisbProduct& b) { return a.timeSec > b.timeSec; });
    out.framesLog.assign(log_.begin(), log_.end());
    out.aircraftCount = (uint32_t)ac_.size();
    out.withPosition = 0;
    out.fisbCount = (uint32_t)out.fisb.size();
    out.tisbCount = 0;
    out.otherCount = 0;
    for (const auto& l : log_) {
        if (l.kind == "TIS-B") out.tisbCount++;
        else if (l.kind == "UAT") out.otherCount++;
    }
    std::vector<std::pair<double, UatAircraft>> rows;
    for (auto& kv : ac_) rows.push_back({kv.second.last, kv.second.ac});
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& r : rows) {
        if (r.second.hasPos) out.withPosition++;
        out.aircraft.push_back(r.second);
        if (out.aircraft.size() >= 128) break;
    }
    out.refValid = refValid_;
    out.refLat = refLat_;
    out.refLon = refLon_;
    out.msgsPerSec = (float)goodWindow_;
    out.frameCount = good_;
}

std::string UatTracker::saveResults() const {
    std::vector<UatAircraft> v;
    for (auto& kv : ac_) v.push_back(kv.second.ac);
    return results::pack(0.0, v);
}
bool UatTracker::loadResults(const std::string& blob, double now) {
    double t0 = 0;
    std::vector<UatAircraft> v;
    if (!results::unpack(blob, t0, v)) return false;
    for (auto& a : v) { Rec r; r.ac = a; r.last = now; ac_[a.icao] = r; }
    return true;
}

} // namespace dect2
