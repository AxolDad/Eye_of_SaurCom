#include "dect2/geo/station_location.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "dect2/automation/json_mini.h"
#include "dect2/geo/maidenhead.h"
#include "../net/tcp_client.h"

// User-facing strings are plain English: core has no translation marker (tr()/N_() live in app/).
namespace dect2::geo {

bool validPos(double lat, double lon) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) return false;
    if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return false;
    return !(lat == 0 && lon == 0);
}
bool validAlt(double m) { return std::isfinite(m) && m >= -500 && m <= 9000; }
bool StationPos::valid() const { return validPos(lat, lon); }

// ---------------------------------------------------------------------------------------------------------------- pasted text
namespace {

const char* kMinus = "\xE2\x88\x92";   // U+2212
const char* kDash = "\xE2\x80\x94";    // U+2014

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

// A plain decimal number at s[i] ([+-]digits[.digits]); advances i. False when there is none.
bool number(const std::string& s, size_t& i, double& v) {
    size_t j = i;
    if (j < s.size() && (s[j] == '-' || s[j] == '+')) j++;
    const size_t d0 = j;
    while (j < s.size() && std::isdigit((unsigned char)s[j])) j++;
    size_t nd = j - d0;
    if (j < s.size() && s[j] == '.') { j++; const size_t f0 = j; while (j < s.size() && std::isdigit((unsigned char)s[j])) j++; nd += j - f0; }
    if (nd == 0) return false;
    v = std::strtod(s.substr(i, j - i).c_str(), nullptr);
    i = j;
    return true;
}
// "a<sep>b" at s[i]
bool pairAt(const std::string& s, size_t i, char sep, double& a, double& b) {
    return number(s, i, a) && i < s.size() && s[i] == sep && (++i, number(s, i, b));
}
size_t findAt(const std::string& s, const char* key, size_t from = 0) { return s.find(key, from); }

struct Found { double lat = NAN, lon = NAN, alt = NAN; bool hasAlt = false; };

// Coordinates inside a map link or geo: URI; false when there are none. The range is not checked here.
bool linkCoords(const std::string& raw, Found& f) {
    std::string s;   // %2C is the comma of ?q=lat%2Clon
    for (size_t i = 0; i < raw.size(); i++) {
        if (raw[i] == '%' && i + 2 < raw.size() + 0 && raw[i + 1] == '2' && (raw[i + 2] == 'C' || raw[i + 2] == 'c')) { s += ','; i += 2; }
        else s += raw[i];
    }
    const std::string l = lower(s);
    if (startsWith(l, "geo:")) {
        std::string body = s.substr(4);
        const size_t cut = body.find_first_of(";?");
        if (cut != std::string::npos) body.resize(cut);
        std::vector<double> v;
        size_t i = 0;
        while (i < body.size()) {
            double x;
            if (!number(body, i, x)) return false;
            v.push_back(x);
            if (i == body.size()) break;
            if (body[i] != ',') return false;
            i++;
        }
        if (v.size() < 2 || v.size() > 3) return false;
        f.lat = v[0]; f.lon = v[1];
        if (v.size() == 3) { f.alt = v[2]; f.hasAlt = true; }
        if (f.lat == 0 && f.lon == 0 && cut != std::string::npos) {   // Android: geo:0,0?q=lat,lon(label)
            const std::string q = lower(s.substr(4 + cut));
            for (size_t p = q.find("q="); p != std::string::npos; p = q.find("q=", p + 1))
                if (pairAt(s, 4 + cut + p + 2, ',', f.lat, f.lon)) { f.hasAlt = false; return true; }
        }
        return true;
    }
    // OpenStreetMap mlat=..&mlon=..
    for (size_t p = findAt(l, "mlat="); p != std::string::npos; p = findAt(l, "mlat=", p + 1)) {
        size_t i = p + 5;
        double la, lo;
        if (!number(s, i, la)) continue;
        const size_t q = findAt(l, "mlon=", i);
        if (q == std::string::npos) continue;
        size_t j = q + 5;
        if (number(s, j, lo)) { f.lat = la; f.lon = lo; return true; }
    }
    // Google, most specific first: the place's own !3dlat!4dlon, then the searched ?q= / query=, then ?ll=, and only then the map centre
    // /@lat,lon,zoom (which is where the view is, not the pin)
    for (size_t p = findAt(s, "!3d"); p != std::string::npos; p = findAt(s, "!3d", p + 1)) {
        size_t i = p + 3;
        double la, lo;
        if (!number(s, i, la) || s.compare(i, 3, "!4d") != 0) continue;
        i += 3;
        if (number(s, i, lo)) { f.lat = la; f.lon = lo; return true; }
    }
    for (const char* key : {"q=", "query=", "ll=", "center="}) {
        const size_t kl = std::strlen(key);
        for (size_t p = findAt(l, key); p != std::string::npos; p = findAt(l, key, p + 1)) {
            if (p == 0 || !std::strchr("?&#;", s[p - 1])) continue;   // only as a parameter: "sll=" is not "ll="
            if (pairAt(s, p + kl, ',', f.lat, f.lon)) return true;
        }
    }
    for (size_t p = s.find('@'); p != std::string::npos; p = s.find('@', p + 1))
        if (pairAt(s, p + 1, ',', f.lat, f.lon)) return true;
    // OpenStreetMap #map=zoom/lat/lon
    for (size_t p = findAt(l, "map="); p != std::string::npos; p = findAt(l, "map=", p + 1)) {
        size_t i = p + 4;
        double z, la, lo;
        if (!number(s, i, z) || i >= s.size() || s[i] != '/') continue;
        i++;
        if (pairAt(s, i, '/', la, lo)) { f.lat = la; f.lon = lo; return true; }
    }
    return false;
}

bool looksLikeLink(const std::string& s) {
    const std::string l = lower(s);
    return startsWith(l, "geo:") || l.find("://") != std::string::npos || l.find("www.") != std::string::npos || l.find("goo.gl") != std::string::npos ||
           l.find("google.") != std::string::npos || l.find("openstreetmap.") != std::string::npos || l.find("osm.org") != std::string::npos;
}

// "sa00", "KO24p": something that is plainly meant as a locator, so a mistake in it gets a locator message
bool looksLikeLocator(const std::string& s) {
    if (s.size() < 2 || s.size() > 10) return false;
    for (char c : s) if (!std::isalnum((unsigned char)c)) return false;
    if (!std::isalpha((unsigned char)s[0]) || !std::isalpha((unsigned char)s[1])) return false;
    return s.size() == 2 || std::isdigit((unsigned char)s[2]);
}

// range checks with a reason
bool checkPair(double lat, double lon, std::string& err) {
    if (!std::isfinite(lat) || !std::isfinite(lon)) { err = "Those numbers are not a position"; return false; }
    if (lat < -90 || lat > 90) {
        if (std::fabs(lat) <= 180 && std::fabs(lon) <= 90) err = std::string("Latitude must be ") + kMinus + "90..90 " + kDash + " are latitude and longitude swapped?";
        else err = std::string("Latitude must be ") + kMinus + "90..90";
        return false;
    }
    if (lon < -180 || lon > 180) { err = std::string("Longitude must be ") + kMinus + "180..180"; return false; }
    if (lat == 0 && lon == 0) { err = "0, 0 is in the ocean; did you mean to set a location?"; return false; }
    return true;
}

bool hasDecimalComma(const std::string& s) {
    if (s.find('.') != std::string::npos) return false;
    for (size_t i = 1; i + 1 < s.size(); i++)
        if (s[i] == ',' && std::isdigit((unsigned char)s[i - 1]) && std::isdigit((unsigned char)s[i + 1])) return true;
    return false;
}

}  // namespace

bool parseLocationText(const std::string& textIn, StationPos& out, std::string& err) {
    const std::string text = trim(textIn);
    err.clear();
    if (text.empty()) { err = "Enter a location: coordinates, a locator or a map link"; return false; }

    double lat = NAN, lon = NAN, alt = NAN;
    bool hasAlt = false, locator = false;

    if (looksLikeLink(text)) {
        Found f;
        if (!linkCoords(text, f)) {
            err = "No coordinates found in that link (open the map, then copy the address with @lat,lon or the geo: text)";
            return false;
        }
        lat = f.lat; lon = f.lon; alt = f.alt; hasAlt = f.hasAlt;
    } else if (looksLikeLocator(text)) {
        if (text.size() != 4 && text.size() != 6 && text.size() != 8) {
            err = "A locator has 4, 6 or 8 characters (like JO02, IO91wm or JN58td), not " + std::to_string(text.size());
            return false;
        }
        if (!fromMaidenhead(text, lat, lon)) {
            err = "'" + text + "' is not a valid locator (like JO02, IO91wm or JN58td)";
            return false;
        }
        locator = true;
    } else {
        if (hasAmbiguousDash(text)) { err = "Is that a minus sign or a separator? Write 52.1, -1.3 or 52.1 1.3"; return false; }
        if (hasDecimalComma(text)) { err = "Use a dot for decimals: 52.1, -1.3"; return false; }
        if (!parseCoordinates(text, lat, lon, false)) {
            err = "Can't read a location from that. Try 52.1, -1.3 or a locator like IO91wm";
            return false;
        }
    }
    if (!checkPair(lat, lon, err)) return false;
    if (hasAlt && !validAlt(alt)) { err = "Altitude must be " + std::string(kMinus) + "500..9000 m"; return false; }
    out.lat = lat; out.lon = lon;
    if (hasAlt) out.altM = alt;
    out.source = locator ? "locator" : "manual";
    return true;
}

StationPos envLocation(std::string* err) {
    if (err) err->clear();
    auto get = [](const char* k) { const char* v = std::getenv(k); return v ? trim(v) : std::string(); };
    const std::string loc = get("SAURCOM_LOCATOR"), lat = get("SAURCOM_LAT"), lon = get("SAURCOM_LON"), alt = get("SAURCOM_ALT");
    auto fail = [&](const std::string& why) { if (err) *err = why; return StationPos(); };
    StationPos p;
    std::string e;
    if (!loc.empty()) {
        if (!parseLocationText(loc, p, e)) return fail("SAURCOM_LOCATOR: " + e);
    } else if (!lat.empty() || !lon.empty()) {
        if (lat.empty()) return fail("SAURCOM_LAT is not set (SAURCOM_LON needs it)");
        if (lon.empty()) return fail("SAURCOM_LON is not set (SAURCOM_LAT needs it)");
        if (!parseLocationText(lat + ", " + lon, p, e)) return fail("SAURCOM_LAT / SAURCOM_LON: " + e);
    } else {
        return StationPos();
    }
    if (!alt.empty()) {
        size_t i = 0;
        double m = 0;
        if (!number(alt, i, m) || i != alt.size() || !validAlt(m)) return fail("SAURCOM_ALT must be a number of metres from -500 to 9000");
        p.altM = m;
    }
    p.source = "env";
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------- NMEA
namespace {

// ddmm.mmmm (latitude, degDigits 2) or dddmm.mmmm (longitude, 3) with its hemisphere letter
bool nmeaAngle(const std::string& v, const std::string& h, int degDigits, const char* pos, const char* neg, double& out) {
    if (v.empty() || h.size() != 1) return false;
    const size_t dot = v.find('.');
    const size_t intLen = dot == std::string::npos ? v.size() : dot;
    if (intLen != (size_t)degDigits + 2) return false;
    for (size_t i = 0; i < v.size(); i++) if (i != dot && !std::isdigit((unsigned char)v[i])) return false;
    const double deg = std::atoi(v.substr(0, degDigits).c_str());
    const double min = std::strtod(v.c_str() + degDigits, nullptr);
    if (min >= 60) return false;
    double r = deg + min / 60;
    if (h[0] == pos[0]) {}
    else if (h[0] == neg[0]) r = -r;
    else return false;
    out = r;
    return true;
}

}  // namespace

bool gpsFromNmea(const std::string& lineIn, GpsFix& io) {
    std::string line = trim(lineIn);
    if (line.size() < 8 || line[0] != '$') return false;
    const size_t star = line.find('*');
    const std::string body = line.substr(1, star == std::string::npos ? std::string::npos : star - 1);
    if (star != std::string::npos) {
        if (line.size() != star + 3) return false;
        unsigned want = 0;
        for (size_t i = star + 1; i < line.size(); i++) {
            if (!std::isxdigit((unsigned char)line[i])) return false;
            want = want * 16 + (unsigned)(std::isdigit((unsigned char)line[i]) ? line[i] - '0' : std::tolower((unsigned char)line[i]) - 'a' + 10);
        }
        unsigned x = 0;
        for (char c : body) x ^= (unsigned char)c;
        if (x != want) return false;
    }
    std::vector<std::string> f;
    for (size_t p = 0;;) {
        const size_t q = body.find(',', p);
        f.push_back(body.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    if (f[0].size() < 5) return false;
    const std::string type = f[0].substr(f[0].size() - 3);
    double lat = 0, lon = 0;
    if (type == "GGA" && f.size() >= 10) {
        const int quality = std::atoi(f[6].c_str());
        const int sats = f[7].empty() ? 0 : std::atoi(f[7].c_str());
        if (quality <= 0) { io.fix = false; io.sats = sats; return false; }
        if (!nmeaAngle(f[2], f[3], 2, "N", "S", lat) || !nmeaAngle(f[4], f[5], 3, "E", "W", lon) || !validPos(lat, lon)) return false;
        char* e = nullptr;
        double a = f[9].empty() ? NAN : std::strtod(f[9].c_str(), &e);
        if (!f[9].empty() && *e) a = NAN;   // not a number: unknown, not 0
        io.fix = true; io.lat = lat; io.lon = lon; io.sats = sats;
        io.altM = validAlt(a) ? a : NAN;
        io.mode = std::isnan(io.altM) ? 2 : 3;
        return true;
    }
    if (type == "RMC" && f.size() >= 7) {
        if (f[2] != "A") return false;
        if (!nmeaAngle(f[3], f[4], 2, "N", "S", lat) || !nmeaAngle(f[5], f[6], 3, "E", "W", lon) || !validPos(lat, lon)) return false;
        io.fix = true; io.lat = lat; io.lon = lon;
        if (io.mode < 2) io.mode = 2;
        return true;
    }
    return false;
}

bool gpsFromGpsdJson(const std::string& line, GpsFix& io) {
    using automation::json::Value;
    Value v;
    std::string e;
    if (line.empty() || !automation::json::parse(line, v, e) || v.type != Value::Obj) return false;
    const std::string cls = v.str("class");
    if (cls == "SKY") {
        if (const Value* u = v.get("uSat"); u && u->type == Value::Num) { io.sats = std::max(0, (int)u->n); return false; }
        if (const Value* s = v.get("satellites"); s && s->type == Value::Arr) {
            int n = 0;
            for (const Value& x : s->a) if (x.flag("used")) n++;
            io.sats = n;
        }
        return false;
    }
    if (cls != "TPV") return false;
    const int mode = (int)v.num("mode", 0);
    io.mode = mode;
    const Value *la = v.get("lat"), *lo = v.get("lon");
    if (mode < 2 || !la || !lo || la->type != Value::Num || lo->type != Value::Num || !validPos(la->n, lo->n)) { io.fix = false; return false; }
    io.fix = true; io.lat = la->n; io.lon = lo->n;
    io.altM = NAN;
    if (mode >= 3)
        for (const char* k : {"altMSL", "alt", "altHAE"})
            if (const Value* a = v.get(k); a && a->type == Value::Num && validAlt(a->n)) { io.altM = a->n; break; }
    return true;
}

// ------------------------------------------------------------------------------------------------------------------ gpsd client
namespace {

bool wasTimeout() {
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

std::string fixText(const GpsFix& f) {
    std::string s = std::string("fix (") + (f.mode >= 3 ? "3D" : "2D");
    if (f.sats > 0) s += ", " + std::to_string(f.sats) + " sats";
    return s + ")";
}

std::string reasonOf(const std::string& err) {
    if (err.find("refused") != std::string::npos) return "connection refused (is gpsd running?)";
    if (err.find("within") != std::string::npos) return "no answer (timed out)";
    if (err.find("cannot find") != std::string::npos) return "unknown host";
    return "connection failed";
}

}  // namespace

bool GpsdClient::start(const std::string& host, int port) {
    if (host.empty() || port < 1 || port > 65535) return false;
    stop();
    {
        std::lock_guard<std::mutex> g(m_);
        stop_ = false;
        fix_ = GpsFix();
        status_ = "connecting";
    }
    th_ = std::thread([this, host, port] { run(host, port); });
    return true;
}

void GpsdClient::stop() {
    {
        std::lock_guard<std::mutex> g(m_);
        stop_ = true;
        if (sock_ >= 0) sockShutdown((sock_t)sock_);
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
    std::lock_guard<std::mutex> g(m_);
    if (!status_.empty()) status_ = "stopped";
    fix_ = GpsFix();
}

bool GpsdClient::latest(GpsFix& out) const {
    std::lock_guard<std::mutex> g(m_);
    if (!fix_.fix || std::chrono::steady_clock::now() - fixAt_ > std::chrono::seconds(10)) return false;   // gpsd repeats TPV about once a second
    out = fix_;
    return true;
}

std::string GpsdClient::status() const {
    std::lock_guard<std::mutex> g(m_);
    if (status_.rfind("fix (", 0) == 0 && std::chrono::steady_clock::now() - fixAt_ > std::chrono::seconds(10)) return "no fix for 10 s";
    return status_.empty() ? "not started" : status_;
}

void GpsdClient::run(std::string host, int port) {
    const std::string where = host + ":" + std::to_string(port);
    int backoff = 1;
    for (;;) {
        {
            std::lock_guard<std::mutex> g(m_);
            if (stop_) return;
            status_ = "connecting";
        }
        std::string err;
        const sock_t s = tcpConnect(host, port, 1000, err);
        if (s == kBadSock) {
            std::unique_lock<std::mutex> lk(m_);
            if (stop_) return;
            status_ = "can't reach " + where + ": " + reasonOf(err);
            fix_ = GpsFix();
            cv_.wait_for(lk, std::chrono::seconds(backoff), [this] { return stop_; });
            if (stop_) return;
            backoff = std::min(backoff * 2, 30);
            continue;
        }
        {
            std::lock_guard<std::mutex> g(m_);
            if (stop_) { sockClose(s); return; }
            sock_ = (long long)s;
            status_ = "connected, no fix yet";
            fix_ = GpsFix();
            fixAt_ = std::chrono::steady_clock::now();
        }
        static const char kWatch[] = "?WATCH={\"enable\":true,\"json\":true}\n";
        bool gotData = false;
        if (sockSendAll(s, kWatch, sizeof kWatch - 1)) {
            sockRecvTimeout(s, 500);
            GpsFix cur;
            std::string buf;
            char chunk[4096];
            for (;;) {
                {
                    std::lock_guard<std::mutex> g(m_);
                    if (stop_) break;
                }
                const long long n = sockRecv(s, chunk, sizeof chunk);
                if (n == 0) break;
                if (n < 0) { if (wasTimeout()) continue; break; }
                gotData = true;
                buf.append(chunk, (size_t)n);
                size_t nl;
                while ((nl = buf.find('\n')) != std::string::npos) {
                    std::string ln = buf.substr(0, nl);
                    buf.erase(0, nl + 1);
                    if (!ln.empty() && ln.back() == '\r') ln.pop_back();
                    gpsFromGpsdJson(ln, cur);
                    std::lock_guard<std::mutex> g(m_);
                    fix_ = cur;
                    if (cur.fix) fixAt_ = std::chrono::steady_clock::now();
                    status_ = cur.fix ? fixText(cur) : "connected, no fix yet";
                }
                if (buf.size() > 65536) buf.clear();   // a line that never ends
            }
        }
        std::unique_lock<std::mutex> lk(m_);
        sock_ = -1;
        sockClose(s);
        fix_ = GpsFix();
        if (stop_) return;
        status_ = "can't reach " + where + ": connection closed";
        if (gotData) backoff = 1;
        cv_.wait_for(lk, std::chrono::seconds(backoff), [this] { return stop_; });
        if (stop_) return;
        backoff = std::min(backoff * 2, 30);
    }
}


// ---------------------------------------------------------------------------------------------------------------------- helpers
double distanceM(double lat1, double lon1, double lat2, double lon2) {
    const double r = 6371008.8, d2r = 3.14159265358979323846 / 180.0;
    const double p1 = lat1 * d2r, p2 = lat2 * d2r, dp = (lat2 - lat1) * d2r, dl = (lon2 - lon1) * d2r;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * r * std::asin(std::min(1.0, std::sqrt(h)));
}

bool followShouldMove(bool haveCurrent, double curLat, double curLon, double lat, double lon, double nowSec, double lastApplySec) {
    if (!validPos(lat, lon)) return false;
    if (lastApplySec >= 0 && nowSec - lastApplySec < 10.0) return false;
    if (haveCurrent && validPos(curLat, curLon) && distanceM(curLat, curLon, lat, lon) <= 20.0) return false;
    return true;
}

StationPos stationFromStored(const StoredStation& s) {
    StationPos p;
    if (!s.hasLat || !s.hasLon || !validPos(s.lat, s.lon)) return p;
    p.lat = s.lat; p.lon = s.lon;
    if (s.hasAlt) { if (validAlt(s.altM)) p.altM = s.altM; }   // a stored but out-of-range value means "not known"
    else if (s.hasOldAlt && validAlt(s.oldAltM)) p.altM = s.oldAltM;
    if (s.hasAnt && validAlt(s.antM) && s.antM >= 0) p.antM = s.antM;
    p.source = s.source.empty() ? "legacy" : s.source;
    return p;
}

}
