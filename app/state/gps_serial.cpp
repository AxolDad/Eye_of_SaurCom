#include "gps_serial.h"
#include "dect2/common/thread_name.h"
#include <cstdio>
#ifndef _WIN32
#include <termios.h>
#include <unistd.h>
#endif

std::string gpsFixText(const dect2::geo::GpsFix& f) {
    std::string s = f.mode >= 3 ? "fix (3D" : "fix (2D";
    if (f.sats > 0) s += ", " + std::to_string(f.sats) + " sats";
    return s + ")";
}

bool SerialNmea::start(const std::string& path, int baud) {
    stop();
    if (path.empty()) return false;
    {
        std::lock_guard<std::mutex> l(m_);
        fix_ = dect2::geo::GpsFix();
        status_ = "opening";
    }
    cancel_ = false;
    running_ = true;
    th_ = dect2::namedThread("saurcom-nmea", [this, path, baud] { run(path, baud); });
    return true;
}

void SerialNmea::stop() {
    cancel_ = true;
    if (th_.joinable()) th_.join();
    running_ = false;
}

bool SerialNmea::latest(dect2::geo::GpsFix& out) const {
    std::lock_guard<std::mutex> l(m_);
    if (!fix_.fix || std::chrono::steady_clock::now() - fixAt_ > std::chrono::seconds(10)) return false;
    out = fix_;
    return true;
}

std::string SerialNmea::status() const {
    std::lock_guard<std::mutex> l(m_);
    if (fix_.fix && std::chrono::steady_clock::now() - fixAt_ <= std::chrono::seconds(10)) return gpsFixText(fix_);
    return status_.empty() ? "not started" : status_;
}

void SerialNmea::run(std::string path, int baud) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        std::lock_guard<std::mutex> l(m_);
        status_ = "can't open " + path;
        running_ = false;
        return;
    }
#ifndef _WIN32
    if (isatty(fileno(f))) {   // a serial GPS: raw 8N1; a read gives up after 0.2 s so that the thread can be stopped
        speed_t sp = B9600;
        switch (baud) { case 4800: sp = B4800; break; case 19200: sp = B19200; break; case 38400: sp = B38400; break;
                        case 57600: sp = B57600; break; case 115200: sp = B115200; break; default: break; }
        termios t;
        if (tcgetattr(fileno(f), &t) == 0) {
            cfmakeraw(&t); cfsetspeed(&t, sp); t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 2;
            tcsetattr(fileno(f), TCSANOW, &t);
        }
    }
#else
    (void)baud;
#endif
    { std::lock_guard<std::mutex> l(m_); status_ = "waiting for data"; }
    dect2::geo::GpsFix cur;
    char line[512];
    while (!cancel_.load()) {
        if (!fgets(line, sizeof line, f)) { clearerr(f); std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
        const bool got = dect2::geo::gpsFromNmea(line, cur);
        std::lock_guard<std::mutex> l(m_);
        if (got) { fix_ = cur; fixAt_ = std::chrono::steady_clock::now(); }
        else {
            if (!cur.fix) fix_ = cur;
            if (line[0] == '$') status_ = "connected, no fix yet";   // sentences arrive: the receiver is there, status() prefers a fresh fix
        }
    }
    fclose(f);
    running_ = false;
}
