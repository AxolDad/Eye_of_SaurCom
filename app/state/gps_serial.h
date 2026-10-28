// A GPS receiver that sends NMEA 0183 sentences over a serial port (or from a file of them), read on its own thread. Used by My station and
// by Radio tools. The sentences are parsed by dect2::geo::gpsFromNmea; a receiver that talks without a fix reports "no fix yet" and no
// position.
#pragma once
#include "dect2/geo/station_location.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

class SerialNmea {
public:
    SerialNmea() = default;
    ~SerialNmea() { stop(); }
    SerialNmea(const SerialNmea&) = delete;
    SerialNmea& operator=(const SerialNmea&) = delete;
    // Opens `path` (a serial port, or a text file of NMEA sentences) at `baud` (ignored for a file) and reads it until stop(). False for an empty path.
    bool start(const std::string& path, int baud = 9600);
    void stop();
    bool running() const { return running_.load(); }
    bool latest(dect2::geo::GpsFix& out) const;   // true with a fix less than 10 s old
    std::string status() const;                   // "opening", "waiting for data", "connected, no fix yet", "fix (3D, 9 sats)", "can't open <path>"
private:
    void run(std::string path, int baud);
    mutable std::mutex m_;
    std::thread th_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    dect2::geo::GpsFix fix_;
    std::chrono::steady_clock::time_point fixAt_{};
    std::string status_;
};

// "fix (3D, 9 sats)" / "fix (2D)": how a fix is described in the status text
std::string gpsFixText(const dect2::geo::GpsFix& f);
