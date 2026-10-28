// StationLocation: pasted-text parsing (every accepted format, every refusal leaves the output alone), validation, NMEA, gpsd JSON, the
// environment, and the gpsd client against a tiny local listening socket
#include "dect2/geo/station_location.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace dect2;
using namespace dect2::geo;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

static void setEnv(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}
static std::string getEnv(const char* k) { const char* v = getenv(k); return v ? v : "\x01unset"; }

static std::string nmea(const std::string& body) {
    unsigned x = 0;
    for (char c : body) x ^= (unsigned char)c;
    char b[8];
    snprintf(b, sizeof b, "*%02X", x);
    return "$" + body + b;
}

// text parses to lat/lon (and source)
static void ok(const char* text, double lat, double lon, const char* src = "manual", double eps = 1e-6) {
    StationPos p;
    std::string err;
    const bool r = parseLocationText(text, p, err);
    CHECK(r, "'%s' refused: %s", text, err.c_str());
    if (!r) return;
    CHECK(near(p.lat, lat, eps) && near(p.lon, lon, eps), "'%s' gave %.7f %.7f, want %.7f %.7f", text, p.lat, p.lon, lat, lon);
    CHECK(p.source == src, "'%s' source '%s' want '%s'", text, p.source.c_str(), src);
}
// text is refused with a one-line error containing `part`, and the output is untouched
static void bad(const char* text, const char* part) {
    StationPos p;
    p.lat = 11; p.lon = 22; p.altM = 33; p.antM = 4; p.source = "legacy";
    std::string err;
    const bool r = parseLocationText(text, p, err);
    CHECK(!r, "'%s' should be refused", text);
    CHECK(!err.empty() && err.find('\n') == std::string::npos, "'%s' error not one line: '%s'", text, err.c_str());
    CHECK(err.find(part) != std::string::npos, "'%s' error '%s' lacks '%s'", text, err.c_str(), part);
    CHECK(p.lat == 11 && p.lon == 22 && p.altM == 33 && p.antM == 4 && p.source == "legacy", "'%s' changed out", text);
}

static void testParse() {
    // decimal pairs
    ok("52.1, -1.3", 52.1, -1.3);
    ok("52.1 -1.3", 52.1, -1.3);
    ok("52.1,-1.3", 52.1, -1.3);
    ok("  52.1N 1.3W ", 52.1, -1.3);
    ok("52.1\xC2\xB0 N, 1.3\xC2\xB0 W", 52.1, -1.3);
    ok("N52.1 W1.3", 52.1, -1.3);
    ok("-33.9 151.2", -33.9, 151.2);
    ok("52, -1", 52, -1);
    // DMS, DM
    const double dlat = 52 + 6 / 60.0, dlon = -(1 + 18 / 60.0);
    ok("52\xC2\xB0" "06'00\"N 1\xC2\xB0" "18'00\"W", dlat, dlon);
    ok("52\xC2\xB0" "06\xE2\x80\xB2" "00\xE2\x80\xB3" "N 1\xC2\xB0" "18\xE2\x80\xB2" "00\xE2\x80\xB3" "W", dlat, dlon);
    ok("52\xC2\xB0" "06\xE2\x80\x99" "00\"N 1\xC2\xB0" "18\xE2\x80\x99" "00\"W", dlat, dlon);
    ok("52 06.000N 001 18.000W", dlat, dlon);
    ok("52 06 36 N 1 18 18 W", 52 + 6 / 60.0 + 36 / 3600.0, -(1 + 18 / 60.0 + 18 / 3600.0));
    // locators: the centre of the square
    ok("KO24", 54.5, 25, "locator");
    ok("ko24pq", 54 + 16.5 / 24, 24 + 15.5 / 12, "locator");
    ok("IO91wm", 51 + 12.5 / 24, -2 + 22.5 / 12 + 0, "locator");
    ok("KO24pq34", 54 + 16.0 / 24 + 4.5 / 240, 24 + 15.0 / 12 + 3.5 / 120, "locator");
    ok(" jn58TD ", 8 + 3.5 / 24 + 40, 10 + 19.5 / 12, "locator");
    // Google links
    ok("https://www.google.com/maps/@52.1234,-1.3456,17z", 52.1234, -1.3456);
    ok("https://www.google.com/maps/place/Home/@52.1234,-1.3456,15z/data=!3m1!1e3", 52.1234, -1.3456);
    ok("https://maps.google.com/?q=52.1234,-1.3456", 52.1234, -1.3456);
    ok("https://maps.google.com/?q=52.1234%2C-1.3456&z=12", 52.1234, -1.3456);
    ok("https://maps.google.com/maps?ll=52.1234,-1.3456&z=10", 52.1234, -1.3456);
    ok("https://www.google.com/maps?foo=1&q=-33.9,151.2", -33.9, 151.2);
    ok("https://www.google.com/maps/place/Home/@52.10,-1.30,12z/data=!3m1!4b1!3d52.2!4d-1.4", 52.2, -1.4);   // the pin, not the view
    ok("https://www.google.com/maps/@52.1,-1.3,12z?q=53,-2", 53, -2);
    ok("https://www.google.com/maps?sll=10.5,20.5&q=52.1,-1.3", 52.1, -1.3);
    ok("https://www.google.com/maps/search/?api=1&query=52.1234%2C-1.3456", 52.1234, -1.3456);
    ok("https://www.google.com/maps?sll=10.5,20.5&ll=52.1,-1.3", 52.1, -1.3);
    ok("geo:0,0?q=52.1234,-1.3456(Home)", 52.1234, -1.3456);
    // typeset and spaced minus signs
    ok("52.1 \xE2\x88\x92" "1.3", 52.1, -1.3);
    ok("52.1, - 1.3", 52.1, -1.3);
    ok("52.1 \xE2\x80\x93" "1.3", 52.1, -1.3);
    ok("52.1 \xE2\x80\x90" "1.3", 52.1, -1.3);   // U+2010
    ok("52.1; - 1.3", 52.1, -1.3);
    ok("- 33.9, 151.2", -33.9, 151.2);
    ok("\xE2\x88\x92" "33.9 151.2", -33.9, 151.2);
    // OSM
    ok("https://www.openstreetmap.org/#map=17/52.1234/-1.3456", 52.1234, -1.3456);
    ok("https://www.openstreetmap.org/?mlat=52.1234&mlon=-1.3456#map=15/52.2/-1.4", 52.1234, -1.3456);
    ok("https://www.openstreetmap.org/?mlat=52.1234&mlon=-1.3456", 52.1234, -1.3456);
    // geo URIs
    ok("geo:52.1234,-1.3456", 52.1234, -1.3456);
    ok("geo:52.1234,-1.3456;u=35", 52.1234, -1.3456);
    {
        StationPos p; std::string err;
        CHECK(parseLocationText("geo:52.1234,-1.3456,123.5;u=10", p, err) && near(p.altM, 123.5) && near(p.lat, 52.1234), "geo with altitude: %s", err.c_str());
        StationPos q; q.altM = 77;
        CHECK(parseLocationText("52.1, -1.3", q, err) && q.altM == 77, "text without altitude keeps altM");
        StationPos r;
        CHECK(!parseLocationText("geo:52.1,-1.3,99999", r, err) && !err.empty() && std::isnan(r.lat), "bad altitude refused: %s", err.c_str());
    }
    // refusals
    bad("", "Enter");
    bad("   ", "Enter");
    bad("KO2", "locator");
    bad("KO24p", "locator");
    bad("KO24pqzz", "locator");
    bad("SA00", "locator");
    bad("KO24yz", "locator");
    bad("https://www.google.com/maps/place/Home", "coordinates");
    bad("https://maps.app.goo.gl/abc123", "coordinates");
    bad("https://www.openstreetmap.org/", "coordinates");
    bad("geo:", "coordinates");
    bad("52,1 -1,3", "Use a dot for decimals: 52.1, -1.3");
    bad("52,1 N 1,3 W", "dot");
    bad("91, 10", "Latitude");
    bad("10, 181", "Longitude");
    bad("152.1, -1.3", "Latitude must be \xE2\x88\x92" "90..90 \xE2\x80\x94 are latitude and longitude swapped?");
    bad("0, 0", "ocean");
    bad("hello world", "location");
    bad("52.1 -, 1.3", "location");
    bad("52.1 - 1.3", "minus sign or a separator? Write 52.1, -1.3 or 52.1 1.3");
    bad("52.1 \xE2\x80\x94 1.3", "minus sign or a separator");
    bad("10 - 20", "minus sign or a separator");
    bad("52.1N - 1.3W", "minus sign or a separator");
    bad("52.1, 1.3 -", "location");
    bad("https://maps.google.com/?sll=52.1,-1.3", "coordinates");
    bad("52.1", "location");
    bad("https://www.google.com/maps/@95.0,-1.3,17z", "Latitude");
    bad("geo:52.1,-1.3,-9999", "ltitude");
    // a valid pair the other way round is just a position (cannot be told apart)
    ok("-1.3, 152.1", -1.3, 152.1);
}

static void testValid() {
    CHECK(validPos(52.1, -1.3) && validPos(90, 180) && validPos(-90, -180) && validPos(0, 1) && validPos(1, 0), "good positions");
    CHECK(!validPos(0, 0), "0,0 refused");
    CHECK(!validPos(90.0001, 0) && !validPos(0, 180.0001) && !validPos(NAN, 1) && !validPos(1, NAN) && !validPos(INFINITY, 1), "bad positions");
    CHECK(validAlt(-500) && validAlt(9000) && validAlt(0), "good altitudes");
    CHECK(!validAlt(-500.1) && !validAlt(9000.1) && !validAlt(NAN) && !validAlt(INFINITY), "bad altitudes");
    StationPos p;
    CHECK(!p.valid(), "default invalid");
    p.lat = 52; p.lon = -1;
    CHECK(p.valid(), "valid");
}

static void testNmea() {
    GpsFix f;
    CHECK(gpsFromNmea(nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), f), "GGA with fix");
    CHECK(f.fix && near(f.lat, 48 + 7.038 / 60) && near(f.lon, 11 + 31.0 / 60) && near(f.altM, 545.4) && f.sats == 8, "GGA values %.6f %.6f %.1f %d", f.lat, f.lon, f.altM, f.sats);
    // RMC afterwards moves the position, keeps sats and altitude
    CHECK(gpsFromNmea(nmea("GPRMC,123520,A,4808.000,N,01132.000,E,022.4,084.4,230394,003.1,W"), f) && f.fix && near(f.lat, 48 + 8.0 / 60) && f.sats == 8 && near(f.altM, 545.4), "RMC A");
    // southern and western
    GpsFix g;
    CHECK(gpsFromNmea(nmea("GNGGA,000000,3354.000,S,07030.000,W,2,12,0.8,12.0,M,0,M,,"), g) && near(g.lat, -(33 + 54.0 / 60)) && near(g.lon, -(70 + 0.5)) && g.sats == 12, "S/W");
    // no fix: GGA quality 0 clears the fix, keeps the sat count, does not touch the old position
    GpsFix h = f;
    CHECK(!gpsFromNmea(nmea("GPGGA,123521,,,,,0,03,,,M,,M,,"), h) && !h.fix && h.sats == 3 && near(h.lat, 48 + 8.0 / 60), "GGA no fix");
    GpsFix n;
    CHECK(!gpsFromNmea(nmea("GPGGA,123521,4807.038,N,01131.000,E,0,00,,,M,,M,,"), n) && !n.fix && std::isnan(n.lat), "GGA quality 0 with coordinates is still no fix");
    // RMC V ignored
    GpsFix v = f;
    CHECK(!gpsFromNmea(nmea("GPRMC,123522,V,4900.000,N,01200.000,E,,,230394,,"), v) && v.fix && near(v.lat, 48 + 8.0 / 60), "RMC V ignored");
    GpsFix v2;
    CHECK(!gpsFromNmea(nmea("GPRMC,123522,V,,,,,,,230394,,"), v2) && !v2.fix && std::isnan(v2.lat), "RMC V on empty");
    // bad checksum
    GpsFix c;
    std::string good = nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,");
    std::string badSum = good.substr(0, good.size() - 2) + (good.back() == '0' ? "1" : "0");
    badSum = good.substr(0, good.size() - 1) + (good.back() == '0' ? '1' : '0');
    CHECK(!gpsFromNmea(badSum, c) && !c.fix && std::isnan(c.lat), "bad checksum refused");
    CHECK(!gpsFromNmea(good.substr(0, good.size() - 1), c) && !c.fix, "truncated checksum refused");
    CHECK(gpsFromNmea(good + "\r\n", c) && c.fix, "CRLF ok");
    // other sentences, junk, out of range
    GpsFix o;
    CHECK(!gpsFromNmea(nmea("GPGSV,3,1,11,03,03,111,00"), o) && !o.fix, "other sentence");
    CHECK(!gpsFromNmea("hello", o) && !gpsFromNmea("", o), "junk");
    CHECK(!gpsFromNmea(nmea("GPGGA,123519,9507.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), o) && !o.fix, "latitude 95");
    CHECK(!gpsFromNmea(nmea("GPGGA,123519,4807.038,X,01131.000,E,1,08,0.9,545.4,M,46.9,M,,"), o) && !o.fix, "bad hemisphere");
    CHECK(!gpsFromNmea(nmea("GPGGA,123519,0000.000,N,00000.000,E,1,08,0.9,10.0,M,46.9,M,,"), o) && !o.fix, "0,0 is not a fix");
    GpsFix a;
    CHECK(gpsFromNmea(nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,abc,M,,M,,"), a) && a.fix && std::isnan(a.altM), "unparsable altitude is NaN, not 0");
    CHECK(gpsFromNmea(nmea("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,,M,,M,,"), a) && a.fix && std::isnan(a.altM), "no altitude is NaN");
}

static void testGpsd() {
    GpsFix f;
    CHECK(gpsFromGpsdJson(R"({"class":"TPV","device":"/dev/ttyUSB0","mode":3,"lat":52.1234567,"lon":-1.3456789,"altMSL":123.4,"altHAE":170.0})", f), "TPV mode 3");
    CHECK(f.fix && f.mode == 3 && near(f.lat, 52.1234567) && near(f.lon, -1.3456789) && near(f.altM, 123.4), "TPV values");
    GpsFix a;
    CHECK(gpsFromGpsdJson(R"({"class":"TPV","mode":3,"lat":52.1,"lon":-1.3,"alt":99.5})", a) && near(a.altM, 99.5), "alt");
    GpsFix b;
    CHECK(gpsFromGpsdJson(R"({"class":"TPV","mode":3,"lat":52.1,"lon":-1.3,"altHAE":150.5})", b) && near(b.altM, 150.5), "altHAE");
    GpsFix d;
    CHECK(gpsFromGpsdJson(R"({"class":"TPV","mode":3,"lat":52.1,"lon":-1.3,"altHAE":150.5,"altMSL":100.5,"alt":120})", d) && near(d.altM, 100.5), "MSL preferred");
    GpsFix t;
    CHECK(gpsFromGpsdJson(R"({"class":"TPV","mode":2,"lat":52.1,"lon":-1.3})", t) && t.fix && t.mode == 2 && std::isnan(t.altM), "2D has no altitude");
    // mode 1: no fix, the old position is left alone
    GpsFix n = f;
    CHECK(!gpsFromGpsdJson(R"({"class":"TPV","device":"/dev/ttyUSB0","mode":1})", n) && !n.fix && near(n.lat, 52.1234567), "TPV mode 1");
    GpsFix z;
    CHECK(!gpsFromGpsdJson(R"({"class":"TPV","mode":3,"lat":0,"lon":0})", z) && !z.fix, "0,0");
    CHECK(!gpsFromGpsdJson(R"({"class":"TPV","mode":3,"lat":95,"lon":0})", z) && !z.fix, "latitude 95");
    CHECK(!gpsFromGpsdJson(R"({"class":"TPV","mode":3})", z) && !z.fix, "no coordinates");
    // not TPV
    GpsFix w = f;
    CHECK(!gpsFromGpsdJson(R"({"class":"VERSION","release":"3.25","rev":"3.25","proto_major":3,"proto_minor":15})", w) && w.fix && near(w.lat, 52.1234567), "VERSION");
    CHECK(!gpsFromGpsdJson("not json", w) && !gpsFromGpsdJson("", w) && !gpsFromGpsdJson("[1,2]", w) && w.fix, "junk");
    GpsFix s = f;
    CHECK(!gpsFromGpsdJson(R"({"class":"SKY","uSat":9,"nSat":14})", s) && s.sats == 9 && s.fix, "SKY uSat");
    GpsFix s2;
    CHECK(!gpsFromGpsdJson(R"({"class":"SKY","satellites":[{"PRN":1,"used":true},{"PRN":2,"used":false},{"PRN":3,"used":true}]})", s2) && s2.sats == 2, "SKY count used");
}

static void testEnv() {
    const char* keys[] = {"SAURCOM_LOCATOR", "SAURCOM_LAT", "SAURCOM_LON", "SAURCOM_ALT"};
    std::string saved[4];
    for (int i = 0; i < 4; i++) saved[i] = getEnv(keys[i]);
    auto clear = [&] { for (auto k : keys) setEnv(k, nullptr); };
    std::string err = "x";
    clear();
    StationPos p = envLocation(&err);
    CHECK(!p.valid() && err.empty(), "nothing set: '%s'", err.c_str());
    setEnv("SAURCOM_LOCATOR", "IO91wm");
    p = envLocation(&err);
    CHECK(p.valid() && p.source == "env" && near(p.lat, 51 + 12.5 / 24) && err.empty(), "locator");
    setEnv("SAURCOM_LOCATOR", "IO9");
    p = envLocation(&err);
    CHECK(!p.valid() && !err.empty() && err.find("SAURCOM_LOCATOR") != std::string::npos, "bad locator: '%s'", err.c_str());
    clear();
    setEnv("SAURCOM_LAT", "52.1"); setEnv("SAURCOM_LON", "-1.3"); setEnv("SAURCOM_ALT", "120");
    p = envLocation(&err);
    CHECK(p.valid() && p.source == "env" && near(p.lat, 52.1) && near(p.lon, -1.3) && near(p.altM, 120) && err.empty(), "lat/lon/alt");
    setEnv("SAURCOM_ALT", nullptr);
    p = envLocation(nullptr);
    CHECK(p.valid() && std::isnan(p.altM), "no alt, null err");
    setEnv("SAURCOM_ALT", "12000");
    p = envLocation(&err);
    CHECK(!p.valid() && err.find("SAURCOM_ALT") != std::string::npos, "bad alt: '%s'", err.c_str());
    setEnv("SAURCOM_ALT", "abc");
    p = envLocation(&err);
    CHECK(!p.valid() && !err.empty(), "junk alt");
    setEnv("SAURCOM_ALT", nullptr);
    setEnv("SAURCOM_LAT", "152.1");
    p = envLocation(&err);
    CHECK(!p.valid() && !err.empty(), "lat out of range: '%s'", err.c_str());
    setEnv("SAURCOM_LAT", "52.1"); setEnv("SAURCOM_LON", nullptr);
    p = envLocation(&err);
    CHECK(!p.valid() && err.find("SAURCOM_LON") != std::string::npos, "lat without lon: '%s'", err.c_str());
    setEnv("SAURCOM_LAT", nullptr); setEnv("SAURCOM_LON", "-1.3");
    p = envLocation(&err);
    CHECK(!p.valid() && err.find("SAURCOM_LAT") != std::string::npos, "lon without lat: '%s'", err.c_str());
    setEnv("SAURCOM_LON", "0"); setEnv("SAURCOM_LAT", "0");
    p = envLocation(&err);
    CHECK(!p.valid() && !err.empty(), "0,0");
    clear();
    for (int i = 0; i < 4; i++) setEnv(keys[i], saved[i] == "\x01unset" ? nullptr : saved[i].c_str());
}

// a one-connection listener on an ephemeral loopback port: sends `script` after the client's ?WATCH line, then holds or closes
struct FakeGpsd {
    sock_t ls = kBadSock;
    int port = 0;
    std::thread th;
    std::string got;
    bool open() {
        netInit();
        ls = socket(AF_INET, SOCK_STREAM, 0);
        if (ls == kBadSock) return false;
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return false;
        socklen_t l = sizeof a;
        getsockname(ls, (sockaddr*)&a, &l);
        port = ntohs(a.sin_port);
        return true;
    }
    void serve(std::string script, int holdMs, int connections = 1) {
        th = std::thread([this, script, holdMs, connections] {
            for (int i = 0; i < connections; i++) {
                sock_t c = accept(ls, nullptr, nullptr);
                if (c == kBadSock) return;
                sockRecvTimeout(c, 1000);
                char b[256];
                const long long n = sockRecv(c, b, sizeof b);
                if (n > 0) got.assign(b, (size_t)n);
                sockSendAll(c, script.data(), script.size());
                std::this_thread::sleep_for(std::chrono::milliseconds(holdMs));
                sockClose(c);
            }
        });
    }
    ~FakeGpsd() {
        if (ls != kBadSock) sockShutdown(ls);   // wakes an accept() nobody connected to
        if (th.joinable()) th.join();
        if (ls != kBadSock) sockClose(ls);
    }
};

static bool waitFor(GpsdClient& c, GpsFix& f, int ms) {
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(ms)) {
        if (c.latest(f)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static void testClient() {
    using clk = std::chrono::steady_clock;
    {   // a fix arrives
        FakeGpsd s;
        CHECK(s.open(), "listen");
        s.serve("{\"class\":\"VERSION\",\"release\":\"3.25\"}\n{\"class\":\"SKY\",\"uSat\":9}\n"
                "{\"class\":\"TPV\",\"mode\":3,\"lat\":52.5,\"lon\":-1.5,\"altMSL\":100}\n", 600);
        GpsdClient c;
        CHECK(c.start("127.0.0.1", s.port), "start");
        GpsFix f;
        CHECK(waitFor(c, f, 5000), "no fix arrived; status '%s'", c.status().c_str());
        CHECK(f.fix && near(f.lat, 52.5) && near(f.lon, -1.5) && near(f.altM, 100) && f.sats == 9, "fix values %.3f %.3f %.1f %d", f.lat, f.lon, f.altM, f.sats);
        CHECK(c.status() == "fix (3D, 9 sats)", "status '%s'", c.status().c_str());
        CHECK(s.got.find("?WATCH={\"enable\":true,\"json\":true}") == 0, "WATCH sent: '%s'", s.got.c_str());
        c.stop();
        CHECK(!c.latest(f) && c.status() == "stopped", "stopped: '%s'", c.status().c_str());
        c.stop();   // twice is fine
    }
    {   // connected, no fix yet
        FakeGpsd s;
        CHECK(s.open(), "listen");
        s.serve("{\"class\":\"TPV\",\"mode\":1}\n", 800);
        GpsdClient c;
        c.start("127.0.0.1", s.port);
        const auto t0 = clk::now();
        while (clk::now() - t0 < std::chrono::milliseconds(5000) && c.status() != "connected, no fix yet") std::this_thread::sleep_for(std::chrono::milliseconds(10));
        GpsFix f;
        CHECK(c.status() == "connected, no fix yet" && !c.latest(f), "status '%s'", c.status().c_str());
    }
    {   // nothing listens: the reason is reported, the destructor stops the thread
        int port = 0;
        {   FakeGpsd s; s.open(); port = s.port; }
        GpsdClient c;
        c.start("127.0.0.1", port);
        const auto t0 = clk::now();
        std::string st;
        while (clk::now() - t0 < std::chrono::milliseconds(5000)) {
            st = c.status();
            if (st.rfind("can't reach", 0) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(st.rfind("can't reach 127.0.0.1:" + std::to_string(port) + ": ", 0) == 0, "status '%s'", st.c_str());
    }
    {   // restart after stop, bad arguments
        GpsdClient c;
        CHECK(!c.start("", 2947) && !c.start("127.0.0.1", 0) && !c.start("127.0.0.1", 70000), "bad arguments");
        GpsFix f;
        CHECK(!c.latest(f), "nothing yet");
        CHECK(!c.status().empty(), "status never empty");
    }
}

int main() {
    testParse();
    testValid();
    testNmea();
    testGpsd();
    testEnv();
    testClient();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("station_location: all tests passed\n");
    return 0;
}
