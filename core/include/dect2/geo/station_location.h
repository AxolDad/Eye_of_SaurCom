// The one position of this station: validated coordinates, the text a user pastes to set them (decimal degrees, DMS, a Maidenhead
// locator, a Google or OpenStreetMap link, a geo: URI), the environment, and a GPS receiver (NMEA sentences or a gpsd on TCP).
// Nothing here looks a position up on the internet: the only network traffic is the connection to the gpsd host the user names.
#pragma once
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace dect2::geo {

struct StationPos {
    double lat = NAN, lon = NAN, altM = NAN, antM = NAN;   // degrees, metres above sea level, antenna height above ground
    std::string source;                                    // "manual", "map", "gps", "locator", "env", "legacy"
    bool valid() const;                                    // lat/lon pass validPos
};

// finite, latitude -90..90, longitude -180..180, and not exactly 0,0 (an unset position, in the ocean)
bool validPos(double lat, double lon);
// finite, -500..9000 m
bool validAlt(double m);

// Reads a position from pasted text (formats in the .cpp and the tests). On success sets lat, lon, source ("locator" or "manual") and,
// when the text carries one (geo:lat,lon,alt), altM. On failure returns false with a one-line English `err` and leaves `out` unchanged.
bool parseLocationText(const std::string& text, StationPos& out, std::string& err);

// SAURCOM_LOCATOR, or SAURCOM_LAT + SAURCOM_LON (+ SAURCOM_ALT); source "env". Invalid StationPos when nothing is set or the values are
// bad; for bad values `err` (if given) says why, for nothing set it is cleared.
StationPos envLocation(std::string* err = nullptr);

struct GpsFix {
    bool fix = false;
    double lat = NAN, lon = NAN, altM = NAN;
    int sats = 0;
    int mode = 0;   // gpsd: 1 none, 2 2D, 3 3D; NMEA: 2 without altitude, 3 with
};
// One NMEA 0183 sentence (GGA or RMC, any talker). A sentence with a checksum must match it. Returns true when the sentence gave a
// position (io.fix is then true). GGA with fix quality 0 sets io.fix false and io.sats and returns false; RMC with status V, a sentence
// with a wrong checksum, any other sentence or an invalid position leaves io unchanged and returns false.
bool gpsFromNmea(const std::string& line, GpsFix& io);
// One line of gpsd's JSON. TPV with mode >= 2 and a valid position gives a fix (true); TPV with mode < 2 sets io.fix false (false); SKY
// updates io.sats (false); other classes and malformed lines leave io unchanged (false).
bool gpsFromGpsdJson(const std::string& line, GpsFix& io);

// A gpsd client on its own thread. start() returns at once; the thread connects, sends ?WATCH, reads, and reconnects with a backoff of
// 1 s doubling to 30 s. Never blocks the caller, except stop(), which waits for the thread (up to one connection attempt, about 1 s).
class GpsdClient {
public:
    GpsdClient() = default;
    ~GpsdClient() { stop(); }
    GpsdClient(const GpsdClient&) = delete;
    GpsdClient& operator=(const GpsdClient&) = delete;
    bool start(const std::string& host = "127.0.0.1", int port = 2947);   // false for an empty host or a port outside 1..65535
    void stop();
    bool latest(GpsFix& out) const;   // true with a current fix
    std::string status() const;       // "connecting", "connected, no fix yet", "fix (3D, 9 sats)", "can't reach 127.0.0.1:2947: <reason>"
private:
    void run(std::string host, int port);
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool stop_ = false;
    GpsFix fix_;
    std::string status_;
    long long sock_ = -1;   // the open connection, so stop() can wake a blocked read
};

}
