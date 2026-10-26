// The flight history on disk: see flights_db.h
//
// The aircraft table keeps identity and the user's own dossier; every statistic is computed from the flights when asked, so a figure can
// never drift away from the flights it came from. The indexes are what make that cheap.
#include "dect2/flights/flights_db.h"
#include <sqlite3.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>

namespace dect2::flights {

namespace {

// A prepared statement that always finalises.
class Stmt {
public:
    Stmt(sqlite3* db, const char* sql) { if (db) sqlite3_prepare_v2(db, sql, -1, &s_, nullptr); }
    Stmt(sqlite3* db, const std::string& sql) : Stmt(db, sql.c_str()) {}
    ~Stmt() { if (s_) sqlite3_finalize(s_); }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    operator bool() const { return s_ != nullptr; }
    sqlite3_stmt* get() const { return s_; }
    bool step() { return s_ && sqlite3_step(s_) == SQLITE_ROW; }
    int done() { return s_ ? sqlite3_step(s_) : SQLITE_ERROR; }
    void bind(int i, double v) { sqlite3_bind_double(s_, i, v); }
    void bindI(int i, int64_t v) { sqlite3_bind_int64(s_, i, v); }
    void bind(int i, const std::string& v) { sqlite3_bind_text(s_, i, v.c_str(), (int)v.size(), SQLITE_TRANSIENT); }
    double col(int i) const { return sqlite3_column_double(s_, i); }
    int64_t colI(int i) const { return sqlite3_column_int64(s_, i); }
    std::string colS(int i) const {
        const unsigned char* t = sqlite3_column_text(s_, i);
        return t ? std::string((const char*)t) : std::string();
    }
    bool isNull(int i) const { return sqlite3_column_type(s_, i) == SQLITE_NULL; }
private:
    sqlite3_stmt* s_ = nullptr;
};

std::string join(const std::vector<std::string>& v, char sep) {
    std::string o;
    for (size_t i = 0; i < v.size(); i++) { if (i) o += sep; o += v[i]; }
    return o;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> o;
    size_t p = 0;
    while (p <= s.size()) {
        size_t e = s.find(sep, p);
        if (e == std::string::npos) e = s.size();
        if (e > p) o.push_back(s.substr(p, e - p));
        p = e + 1;
    }
    return o;
}

// local hour of the day and weekday of a Unix time, as the user's clock shows it (the questions are about the user's own days)
void localHourDow(double t, int& hour, int& dow) {
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    localtime_s(&g, &tt);
#else
    localtime_r(&tt, &g);
#endif
    hour = g.tm_hour;
    dow = g.tm_wday;
}

std::string hex6(uint32_t icao) {
    char b[8];
    snprintf(b, sizeof b, "%06X", icao & 0xFFFFFF);
    return b;
}

std::string jsonEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else if ((unsigned char)c < 0x20) continue;
        else o += c;
    }
    return o;
}

std::string csvEsc(const std::string& s) {
    if (s.find_first_of(",\"\n\r") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else o += c; }
    return o + "\"";
}

std::string num(double v, int dp) {
    char b[48];
    snprintf(b, sizeof b, "%.*f", dp, v);
    return b;
}

} // namespace

// ---------------------------------------------------------------- the store

struct Db::Impl {
    mutable std::mutex mu;
    sqlite3* db = nullptr;
    std::string path;
    Limits lim;

    bool exec(const char* sql) const {
        if (!db) return false;
        char* em = nullptr;
        const bool ok = sqlite3_exec(db, sql, nullptr, nullptr, &em) == SQLITE_OK;
        sqlite3_free(em);
        return ok;
    }
    int64_t scalar(const char* sql) const {
        Stmt s(db, sql);
        return s.step() ? s.colI(0) : 0;
    }
    int64_t fileBytes() const { return scalar("PRAGMA page_count") * scalar("PRAGMA page_size"); }

    // ---- the pieces of a WHERE clause every question shares
    // The parameters go in the order they were added, numbers and text interleaved by index, so bind in one pass.
    struct Binder {
        std::vector<std::pair<int, double>> n;
        std::vector<std::pair<int, std::string>> t;
    };
    // builds the clause and records the binding order
    std::string whereBound(const Filter& f, Binder& b) const {
        std::string w;
        int idx = 0;
        auto add = [&](const std::string& c) { w += w.empty() ? " WHERE " : " AND "; w += c; };
        auto nextN = [&](double v) { b.n.push_back({++idx, v}); return "?" + std::to_string(idx); };
        auto nextT = [&](const std::string& v) { b.t.push_back({++idx, v}); return "?" + std::to_string(idx); };
        if (f.since > 0) add("f.t0 >= " + nextN(f.since));
        if (f.until > 0) add("f.t0 <= " + nextN(f.until));
        if (f.icao) add("f.icao = " + std::to_string(f.icao));
        if (f.kind >= 0) add("COALESCE(a.kind_user, f.kind) = " + std::to_string(f.kind));
        if (f.minHour >= 0) add("f.hour >= " + std::to_string(f.minHour));
        if (f.maxHour >= 0) add("f.hour <= " + std::to_string(f.maxHour));
        if (f.dow >= 0) add("f.dow = " + std::to_string(f.dow));
        if (f.emergenciesOnly) add("(f.emergency > 0 OR f.emerg_squawk = 1)");
        if (f.withPositionOnly) add("f.npoints > 0");
        if (!f.registration.empty()) add("a.registration LIKE " + nextT("%" + f.registration + "%"));
        if (!f.callsign.empty()) add("f.callsign LIKE " + nextT(f.callsign + "%"));
        if (!f.op3.empty()) add("f.op3 = " + nextT(f.op3));
        if (!f.airport.empty()) {
            const std::string p = nextT(f.airport);
            add("(f.org_icao = " + p + " OR f.dst_icao = " + p + ")");
        }
        return w;
    }
    static void bindAll(Stmt& s, const Binder& b) {
        for (const auto& kv : b.n) s.bind(kv.first, kv.second);
        for (const auto& kv : b.t) s.bind(kv.first, kv.second);
    }

    // the standard join, so a filter on the aircraft's identity or the user's kind override works everywhere
    static const char* kFrom() { return " FROM flights f LEFT JOIN aircraft a ON a.icao = f.icao"; }

    void readFlight(Stmt& s, Flight& f) const {
        int i = 0;
        f.id = s.colI(i++);
        f.icao = (uint32_t)s.colI(i++);
        f.callsign = s.colS(i++);
        f.squawk = s.colS(i++);
        f.tStart = s.col(i++);
        f.tEnd = s.col(i++);
        f.airborneSec = s.col(i++);
        f.points = (size_t)s.colI(i++);
        f.messages = (uint64_t)s.colI(i++);
        f.maxAltFt = (int)s.colI(i++);
        f.cruiseAltFt = (int)s.colI(i++);
        f.maxSpeedKt = (float)s.col(i++);
        f.cruiseSpeedKt = (float)s.col(i++);
        f.distanceNm = (float)s.col(i++);
        f.minRangeNm = (float)s.col(i++);
        f.maxRangeNm = (float)s.col(i++);
        f.maxVrateFpm = (int)s.colI(i++);
        f.minVrateFpm = (int)s.colI(i++);
        f.origin.kind = (EndKind)s.colI(i++);
        f.origin.icao = s.colS(i++);
        f.origin.runway = s.colS(i++);
        f.destination.kind = (EndKind)s.colI(i++);
        f.destination.icao = s.colS(i++);
        f.destination.runway = s.colS(i++);
        f.approach.ok = s.colI(i++) != 0;
        f.approach.airport = s.colS(i++);
        f.approach.runway = s.colS(i++);
        f.approach.meanLateralNm = (float)s.col(i++);
        f.approach.maxLateralNm = (float)s.col(i++);
        f.approach.meanVerticalFt = (float)s.col(i++);
        f.approach.maxVerticalFt = (float)s.col(i++);
        f.approach.thresholdCrossFt = (float)s.col(i++);
        f.approach.alignedFromNm = (float)s.col(i++);
        for (int k = 0; k < 8; k++) f.phaseSec[k] = (uint32_t)s.colI(i++);
        f.sawGround = s.colI(i++) != 0;
        f.sawTakeoff = s.colI(i++) != 0;
        f.sawLanding = s.colI(i++) != 0;
        f.emergency = (int)s.colI(i++);
        f.emergencySquawk = s.colI(i++) != 0;
        f.squawksSeen = split(s.colS(i++), ' ');
        f.routeSignature = s.colS(i++);
        f.kind = (uint8_t)s.colI(i++);
        f.kindConfidence = (float)s.col(i++);
        f.kindBasis = s.colS(i++);
        f.hasAlt = f.maxAltFt != 0;
        f.hasSpeed = f.maxSpeedKt != 0;
        f.hasRange = f.maxRangeNm != 0;
        f.complete = true;
    }
    static const char* kFlightCols() {
        return "f.id,f.icao,f.callsign,f.squawk,f.t0,f.t1,f.airborne,f.npoints,f.messages,f.max_alt,f.cruise_alt,f.max_speed,f.cruise_speed,"
               "f.distance,f.min_range,f.max_range,f.max_vrate,f.min_vrate,"
               "f.org_kind,f.org_icao,f.org_rwy,f.dst_kind,f.dst_icao,f.dst_rwy,"
               "f.appr_ok,f.appr_airport,f.appr_rwy,f.appr_lat_mean,f.appr_lat_max,f.appr_vert_mean,f.appr_vert_max,f.appr_thr,f.appr_aligned,"
               "f.ph0,f.ph1,f.ph2,f.ph3,f.ph4,f.ph5,f.ph6,f.ph7,"
               "f.saw_ground,f.saw_takeoff,f.saw_landing,f.emergency,f.emerg_squawk,f.squawks,f.route,f.kind,f.kind_conf,f.kind_basis";
    }

    int64_t pruneLocked() {
        if (!db) return 0;
        int64_t gone = 0;
        if (lim.maxAgeDays > 0) {
            const double cut = (double)std::time(nullptr) - lim.maxAgeDays * 86400.0;
            Stmt s(db, "DELETE FROM flights WHERE t0 < ?1");
            s.bind(1, cut);
            s.done();
            gone += sqlite3_changes(db);
        }
        if (lim.maxFlights > 0) {
            const int64_t n = scalar("SELECT COUNT(*) FROM flights");
            if (n > lim.maxFlights) {
                Stmt s(db, "DELETE FROM flights WHERE id IN (SELECT id FROM flights ORDER BY t0 ASC LIMIT ?1)");
                s.bindI(1, n - lim.maxFlights);
                s.done();
                gone += sqlite3_changes(db);
            }
        }
        if (lim.trackMaxAgeDays > 0) {
            const double cut = (double)std::time(nullptr) - lim.trackMaxAgeDays * 86400.0;
            Stmt s(db, "DELETE FROM points WHERE flight IN (SELECT id FROM flights WHERE t0 < ?1)");
            s.bind(1, cut);
            s.done();
        }
        if (lim.maxBytes > 0) {
            // drop the oldest tenth until the file fits, tracks first because they are most of it
            for (int round = 0; round < 12 && fileBytes() > lim.maxBytes; round++) {
                const int64_t n = scalar("SELECT COUNT(*) FROM flights");
                if (n < 10) break;
                Stmt s(db, "DELETE FROM flights WHERE id IN (SELECT id FROM flights ORDER BY t0 ASC LIMIT ?1)");
                s.bindI(1, std::max<int64_t>(1, n / 10));
                s.done();
                gone += sqlite3_changes(db);
                exec("PRAGMA incremental_vacuum");
            }
        }
        if (gone > 0) exec("PRAGMA incremental_vacuum");
        return gone;
    }
};

Db::Db() : p_(new Impl) {}
Db::~Db() { close(); }

bool Db::open(const std::string& path, std::string* err) {
    close();
    std::lock_guard<std::mutex> lk(p_->mu);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (err) *err = db ? sqlite3_errmsg(db) : "out of memory";
        if (db) sqlite3_close(db);
        return false;
    }
    p_->db = db;
    sqlite3_busy_timeout(db, 3000);
    p_->exec("PRAGMA auto_vacuum=INCREMENTAL");
    if (path != ":memory:") { p_->exec("PRAGMA journal_mode=WAL"); p_->exec("PRAGMA synchronous=NORMAL"); }
    p_->exec("PRAGMA foreign_keys=ON");
    const char* schema =
        "CREATE TABLE IF NOT EXISTS flights(id INTEGER PRIMARY KEY, icao INTEGER NOT NULL, callsign TEXT NOT NULL DEFAULT '', squawk TEXT NOT NULL DEFAULT '',"
        " t0 REAL NOT NULL, t1 REAL NOT NULL, airborne REAL NOT NULL DEFAULT 0, npoints INTEGER NOT NULL DEFAULT 0, messages INTEGER NOT NULL DEFAULT 0,"
        " max_alt INTEGER NOT NULL DEFAULT 0, cruise_alt INTEGER NOT NULL DEFAULT 0, max_speed REAL NOT NULL DEFAULT 0, cruise_speed REAL NOT NULL DEFAULT 0,"
        " distance REAL NOT NULL DEFAULT 0, min_range REAL NOT NULL DEFAULT 0, max_range REAL NOT NULL DEFAULT 0,"
        " max_vrate INTEGER NOT NULL DEFAULT 0, min_vrate INTEGER NOT NULL DEFAULT 0,"
        " org_kind INTEGER NOT NULL DEFAULT 0, org_icao TEXT NOT NULL DEFAULT '', org_rwy TEXT NOT NULL DEFAULT '',"
        " dst_kind INTEGER NOT NULL DEFAULT 0, dst_icao TEXT NOT NULL DEFAULT '', dst_rwy TEXT NOT NULL DEFAULT '',"
        " appr_ok INTEGER NOT NULL DEFAULT 0, appr_airport TEXT NOT NULL DEFAULT '', appr_rwy TEXT NOT NULL DEFAULT '',"
        " appr_lat_mean REAL NOT NULL DEFAULT 0, appr_lat_max REAL NOT NULL DEFAULT 0, appr_vert_mean REAL NOT NULL DEFAULT 0,"
        " appr_vert_max REAL NOT NULL DEFAULT 0, appr_thr REAL NOT NULL DEFAULT 0, appr_aligned REAL NOT NULL DEFAULT 0,"
        " ph0 INTEGER NOT NULL DEFAULT 0, ph1 INTEGER NOT NULL DEFAULT 0, ph2 INTEGER NOT NULL DEFAULT 0, ph3 INTEGER NOT NULL DEFAULT 0,"
        " ph4 INTEGER NOT NULL DEFAULT 0, ph5 INTEGER NOT NULL DEFAULT 0, ph6 INTEGER NOT NULL DEFAULT 0, ph7 INTEGER NOT NULL DEFAULT 0,"
        " saw_ground INTEGER NOT NULL DEFAULT 0, saw_takeoff INTEGER NOT NULL DEFAULT 0, saw_landing INTEGER NOT NULL DEFAULT 0,"
        " emergency INTEGER NOT NULL DEFAULT 0, emerg_squawk INTEGER NOT NULL DEFAULT 0, squawks TEXT NOT NULL DEFAULT '',"
        " route TEXT NOT NULL DEFAULT '', kind INTEGER NOT NULL DEFAULT 0, kind_conf REAL NOT NULL DEFAULT 0, kind_basis TEXT NOT NULL DEFAULT '',"
        " op3 TEXT NOT NULL DEFAULT '', hour INTEGER NOT NULL DEFAULT 0, dow INTEGER NOT NULL DEFAULT 0, day INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS flights_t ON flights(t0);"
        "CREATE INDEX IF NOT EXISTS flights_icao_t ON flights(icao, t0);"
        "CREATE INDEX IF NOT EXISTS flights_org ON flights(org_icao);"
        "CREATE INDEX IF NOT EXISTS flights_dst ON flights(dst_icao);"
        "CREATE INDEX IF NOT EXISTS flights_op3 ON flights(op3);"
        "CREATE INDEX IF NOT EXISTS flights_kind ON flights(kind);"
        "CREATE INDEX IF NOT EXISTS flights_route ON flights(icao, route);"
        "CREATE TABLE IF NOT EXISTS points(flight INTEGER NOT NULL REFERENCES flights(id) ON DELETE CASCADE,"
        " t REAL NOT NULL, lat REAL NOT NULL, lon REAL NOT NULL, alt INTEGER, speed REAL, trk REAL, vrate INTEGER,"
        " ground INTEGER NOT NULL DEFAULT 0, phase INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS points_flight ON points(flight, t);"
        "CREATE TABLE IF NOT EXISTS aircraft(icao INTEGER PRIMARY KEY, registration TEXT NOT NULL DEFAULT '', type TEXT NOT NULL DEFAULT '',"
        " operator TEXT NOT NULL DEFAULT '', category TEXT NOT NULL DEFAULT '', country TEXT NOT NULL DEFAULT '',"
        " rotorcraft INTEGER NOT NULL DEFAULT 0, kind INTEGER NOT NULL DEFAULT 0, kind_conf REAL NOT NULL DEFAULT 0,"
        " kind_basis TEXT NOT NULL DEFAULT '', kind_user INTEGER, notes TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '',"
        " pictures TEXT NOT NULL DEFAULT '');"
        "CREATE INDEX IF NOT EXISTS aircraft_reg ON aircraft(registration);"
        "CREATE TABLE IF NOT EXISTS facts(icao INTEGER NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL DEFAULT '', edited REAL NOT NULL DEFAULT 0,"
        " PRIMARY KEY(icao, key));"
        "CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY, icao INTEGER NOT NULL DEFAULT 0, flight INTEGER NOT NULL DEFAULT 0,"
        " t REAL NOT NULL, kind TEXT NOT NULL DEFAULT '', txt TEXT NOT NULL DEFAULT '', user INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS events_t ON events(t);"
        "CREATE INDEX IF NOT EXISTS events_icao ON events(icao, t);";
    char* em = nullptr;
    if (sqlite3_exec(db, schema, nullptr, nullptr, &em) != SQLITE_OK) {
        if (err) *err = em ? em : "schema";
        sqlite3_free(em);
        sqlite3_close(db);
        p_->db = nullptr;
        return false;
    }
    p_->path = path;
    return true;
}

void Db::close() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->db) sqlite3_close(p_->db);
    p_->db = nullptr;
    p_->path.clear();
}
bool Db::isOpen() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->db != nullptr; }
std::string Db::path() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->path; }
void Db::setLimits(const Limits& l) { std::lock_guard<std::mutex> lk(p_->mu); p_->lim = l; }
Limits Db::limits() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->lim; }
int64_t Db::prune() { std::lock_guard<std::mutex> lk(p_->mu); return p_->pruneLocked(); }

void Db::clear() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->exec("DELETE FROM points; DELETE FROM flights; DELETE FROM events; DELETE FROM facts; DELETE FROM aircraft");
    p_->exec("PRAGMA incremental_vacuum");
}

Stats Db::stats() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Stats s;
    if (!p_->db) return s;
    s.flights = p_->scalar("SELECT COUNT(*) FROM flights");
    s.points = p_->scalar("SELECT COUNT(*) FROM points");
    s.aircraft = p_->scalar("SELECT COUNT(*) FROM aircraft");
    s.events = p_->scalar("SELECT COUNT(*) FROM events");
    s.bytes = p_->fileBytes();
    Stmt q(p_->db, "SELECT MIN(t0), MAX(t1) FROM flights");
    if (q.step()) { s.oldest = q.col(0); s.newest = q.col(1); }
    return s;
}

// ---------------------------------------------------------------- writing

bool Db::addFlight(Flight& f, const std::vector<Point>& pts, const AircraftFacts& facts) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    int hour = 0, dow = 0;
    localHourDow(f.tStart, hour, dow);
    std::string op3;
    airlineCallsign(f.callsign, op3);
    p_->exec("BEGIN");
    {
        Stmt s(p_->db,
               "INSERT INTO flights(icao,callsign,squawk,t0,t1,airborne,npoints,messages,max_alt,cruise_alt,max_speed,cruise_speed,"
               "distance,min_range,max_range,max_vrate,min_vrate,org_kind,org_icao,org_rwy,dst_kind,dst_icao,dst_rwy,"
               "appr_ok,appr_airport,appr_rwy,appr_lat_mean,appr_lat_max,appr_vert_mean,appr_vert_max,appr_thr,appr_aligned,"
               "ph0,ph1,ph2,ph3,ph4,ph5,ph6,ph7,saw_ground,saw_takeoff,saw_landing,emergency,emerg_squawk,squawks,route,"
               "kind,kind_conf,kind_basis,op3,hour,dow,day) VALUES("
               "?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,?24,?25,?26,?27,?28,?29,?30,?31,?32,"
               "?33,?34,?35,?36,?37,?38,?39,?40,?41,?42,?43,?44,?45,?46,?47,?48,?49,?50,?51,?52,?53,?54)");
        if (!s) { p_->exec("ROLLBACK"); return false; }
        int i = 1;
        s.bindI(i++, f.icao);
        s.bind(i++, f.callsign);
        s.bind(i++, f.squawk);
        s.bind(i++, f.tStart);
        s.bind(i++, f.tEnd);
        s.bind(i++, f.airborneSec);
        s.bindI(i++, (int64_t)pts.size());
        s.bindI(i++, (int64_t)f.messages);
        s.bindI(i++, f.maxAltFt);
        s.bindI(i++, f.cruiseAltFt);
        s.bind(i++, f.maxSpeedKt);
        s.bind(i++, f.cruiseSpeedKt);
        s.bind(i++, f.distanceNm);
        s.bind(i++, f.minRangeNm);
        s.bind(i++, f.maxRangeNm);
        s.bindI(i++, f.maxVrateFpm);
        s.bindI(i++, f.minVrateFpm);
        s.bindI(i++, (int64_t)f.origin.kind);
        s.bind(i++, f.origin.icao);
        s.bind(i++, f.origin.runway);
        s.bindI(i++, (int64_t)f.destination.kind);
        s.bind(i++, f.destination.icao);
        s.bind(i++, f.destination.runway);
        s.bindI(i++, f.approach.ok ? 1 : 0);
        s.bind(i++, f.approach.airport);
        s.bind(i++, f.approach.runway);
        s.bind(i++, f.approach.meanLateralNm);
        s.bind(i++, f.approach.maxLateralNm);
        s.bind(i++, f.approach.meanVerticalFt);
        s.bind(i++, f.approach.maxVerticalFt);
        s.bind(i++, f.approach.thresholdCrossFt);
        s.bind(i++, f.approach.alignedFromNm);
        for (int k = 0; k < 8; k++) s.bindI(i++, f.phaseSec[k]);
        s.bindI(i++, f.sawGround ? 1 : 0);
        s.bindI(i++, f.sawTakeoff ? 1 : 0);
        s.bindI(i++, f.sawLanding ? 1 : 0);
        s.bindI(i++, f.emergency);
        s.bindI(i++, f.emergencySquawk ? 1 : 0);
        s.bind(i++, join(f.squawksSeen, ' '));
        s.bind(i++, f.routeSignature);
        s.bindI(i++, f.kind);
        s.bind(i++, f.kindConfidence);
        s.bind(i++, f.kindBasis);
        s.bind(i++, op3);
        s.bindI(i++, hour);
        s.bindI(i++, dow);
        s.bindI(i++, (int64_t)std::floor(f.tStart / 86400.0));
        if (s.done() != SQLITE_DONE) { p_->exec("ROLLBACK"); return false; }
    }
    f.id = sqlite3_last_insert_rowid(p_->db);
    if (p_->lim.keepTracks && !pts.empty()) {
        Stmt s(p_->db, "INSERT INTO points(flight,t,lat,lon,alt,speed,trk,vrate,ground,phase) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
        for (const Point& q : pts) {
            sqlite3_reset(s.get());
            s.bindI(1, f.id);
            s.bind(2, q.t);
            s.bind(3, q.lat);
            s.bind(4, q.lon);
            if (q.hasAlt) s.bindI(5, q.altFt); else sqlite3_bind_null(s.get(), 5);
            if (q.hasSpeed) s.bind(6, q.speedKt); else sqlite3_bind_null(s.get(), 6);
            if (q.hasTrack) s.bind(7, q.trackDeg); else sqlite3_bind_null(s.get(), 7);
            if (q.hasVrate) s.bindI(8, q.vrateFpm); else sqlite3_bind_null(s.get(), 8);
            s.bindI(9, q.ground ? 1 : 0);
            s.bindI(10, q.phase);
            s.done();
        }
    }
    // the dossier: identity and the inference, never touching what the user wrote
    const bool firstEver = p_->scalar(("SELECT COUNT(*) FROM aircraft WHERE icao = " + std::to_string(f.icao)).c_str()) == 0;
    {
        Stmt s(p_->db,
               "INSERT INTO aircraft(icao,registration,type,operator,category,country,rotorcraft,kind,kind_conf,kind_basis) "
               "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) "
               "ON CONFLICT(icao) DO UPDATE SET "
               " registration=CASE WHEN ?2 <> '' THEN ?2 ELSE registration END,"
               " type=CASE WHEN ?3 <> '' THEN ?3 ELSE type END,"
               " operator=CASE WHEN ?4 <> '' THEN ?4 ELSE operator END,"
               " category=CASE WHEN ?5 <> '' THEN ?5 ELSE category END,"
               " country=CASE WHEN ?6 <> '' THEN ?6 ELSE country END,"
               " rotorcraft=MAX(rotorcraft, ?7),"
               // a more confident inference replaces a weaker one; the user's override lives in its own column and is untouched
               " kind=CASE WHEN ?9 > kind_conf THEN ?8 ELSE kind END,"
               " kind_basis=CASE WHEN ?9 > kind_conf THEN ?10 ELSE kind_basis END,"
               " kind_conf=MAX(kind_conf, ?9)");
        if (s) {
            s.bindI(1, f.icao);
            s.bind(2, facts.registration);
            s.bind(3, facts.type);
            s.bind(4, facts.operatorName);
            s.bind(5, facts.category);
            s.bind(6, std::string());
            s.bindI(7, facts.rotorcraft ? 1 : 0);
            s.bindI(8, f.kind);
            s.bind(9, f.kindConfidence);
            s.bind(10, f.kindBasis);
            s.done();
        }
    }
    // the events this flight earns
    auto event = [&](const char* kind, const std::string& text, double t) {
        Stmt e(p_->db, "INSERT INTO events(icao,flight,t,kind,txt,user) VALUES(?1,?2,?3,?4,?5,0)");
        if (!e) return;
        e.bindI(1, f.icao);
        e.bindI(2, f.id);
        e.bind(3, t);
        e.bind(4, std::string(kind));
        e.bind(5, text);
        e.done();
    };
    if (firstEver) event("first", "first time heard" + (facts.registration.empty() ? std::string() : " (" + facts.registration + ")"), f.tStart);
    if (f.emergencySquawk || f.emergency > 0) {
        std::string what = f.emergencySquawk ? "squawked " + f.squawk : "emergency state " + std::to_string(f.emergency);
        event("emergency", what, f.tStart);
    }
    p_->exec("COMMIT");
    return true;
}

bool Db::setNotes(uint32_t icao, const std::string& notes) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, "INSERT INTO aircraft(icao,notes) VALUES(?1,?2) ON CONFLICT(icao) DO UPDATE SET notes=?2");
    if (!s) return false;
    s.bindI(1, icao);
    s.bind(2, notes);
    return s.done() == SQLITE_DONE;
}

bool Db::setTags(uint32_t icao, const std::vector<std::string>& tags) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, "INSERT INTO aircraft(icao,tags) VALUES(?1,?2) ON CONFLICT(icao) DO UPDATE SET tags=?2");
    if (!s) return false;
    s.bindI(1, icao);
    s.bind(2, join(tags, '\n'));
    return s.done() == SQLITE_DONE;
}

bool Db::setPictures(uint32_t icao, const std::vector<std::string>& paths) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, "INSERT INTO aircraft(icao,pictures) VALUES(?1,?2) ON CONFLICT(icao) DO UPDATE SET pictures=?2");
    if (!s) return false;
    s.bindI(1, icao);
    s.bind(2, join(paths, '\n'));
    return s.done() == SQLITE_DONE;
}

bool Db::setKindOverride(uint32_t icao, int kind) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, "INSERT INTO aircraft(icao,kind_user) VALUES(?1,?2) ON CONFLICT(icao) DO UPDATE SET kind_user=?2");
    if (!s) return false;
    s.bindI(1, icao);
    if (kind < 0) sqlite3_bind_null(s.get(), 2); else s.bindI(2, kind);
    return s.done() == SQLITE_DONE;
}

bool Db::setFact(uint32_t icao, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db || key.empty()) return false;
    if (value.empty()) {
        Stmt s(p_->db, "DELETE FROM facts WHERE icao = ?1 AND key = ?2");
        if (!s) return false;
        s.bindI(1, icao);
        s.bind(2, key);
        return s.done() == SQLITE_DONE;
    }
    Stmt s(p_->db, "INSERT INTO facts(icao,key,value,edited) VALUES(?1,?2,?3,?4) ON CONFLICT(icao,key) DO UPDATE SET value=?3, edited=?4");
    if (!s) return false;
    s.bindI(1, icao);
    s.bind(2, key);
    s.bind(3, value);
    s.bind(4, (double)std::time(nullptr));
    return s.done() == SQLITE_DONE;
}

bool Db::addEvent(const Event& e) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, "INSERT INTO events(icao,flight,t,kind,txt,user) VALUES(?1,?2,?3,?4,?5,?6)");
    if (!s) return false;
    s.bindI(1, e.icao);
    s.bindI(2, e.flightId);
    s.bind(3, e.t > 0 ? e.t : (double)std::time(nullptr));
    s.bind(4, e.kind);
    s.bind(5, e.text);
    s.bindI(6, e.user ? 1 : 0);
    return s.done() == SQLITE_DONE;
}

bool Db::removeEvent(int64_t id) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, "DELETE FROM events WHERE id = ?1");
    if (!s) return false;
    s.bindI(1, id);
    return s.done() == SQLITE_DONE;
}

// ---------------------------------------------------------------- flights

std::vector<Flight> Db::flights(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Flight> out;
    if (!p_->db) return out;
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    const std::string sql = std::string("SELECT ") + Impl::kFlightCols() + Impl::kFrom() + w +
                            " ORDER BY f.t0 " + (f.oldestFirst ? "ASC" : "DESC") + ", f.id " + (f.oldestFirst ? "ASC" : "DESC") +
                            " LIMIT " + std::to_string(f.limit > 0 ? f.limit : 500) + " OFFSET " + std::to_string(std::max(0, f.offset));
    Stmt s(p_->db, sql);
    if (!s) return out;
    Impl::bindAll(s, b);
    while (s.step()) {
        Flight x;
        p_->readFlight(s, x);
        out.push_back(std::move(x));
    }
    return out;
}

int64_t Db::countFlights(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return 0;
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    Stmt s(p_->db, std::string("SELECT COUNT(*)") + Impl::kFrom() + w);
    if (!s) return 0;
    Impl::bindAll(s, b);
    return s.step() ? s.colI(0) : 0;
}

bool Db::flight(int64_t id, Flight& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, std::string("SELECT ") + Impl::kFlightCols() + Impl::kFrom() + " WHERE f.id = ?1");
    if (!s) return false;
    s.bindI(1, id);
    if (!s.step()) return false;
    p_->readFlight(s, out);
    return true;
}

std::vector<Point> Db::track(int64_t flightId) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Point> out;
    if (!p_->db) return out;
    Stmt s(p_->db, "SELECT t,lat,lon,alt,speed,trk,vrate,ground,phase FROM points WHERE flight = ?1 ORDER BY t ASC");
    if (!s) return out;
    s.bindI(1, flightId);
    while (s.step()) {
        Point p;
        p.t = s.col(0);
        p.lat = s.col(1);
        p.lon = s.col(2);
        if (!s.isNull(3)) { p.altFt = (int)s.colI(3); p.hasAlt = true; }
        if (!s.isNull(4)) { p.speedKt = (float)s.col(4); p.hasSpeed = true; }
        if (!s.isNull(5)) { p.trackDeg = (float)s.col(5); p.hasTrack = true; }
        if (!s.isNull(6)) { p.vrateFpm = (int)s.colI(6); p.hasVrate = true; }
        p.ground = s.colI(7) != 0;
        p.phase = (Phase)s.colI(8);
        out.push_back(p);
    }
    return out;
}

// ---------------------------------------------------------------- aircraft and dossiers

namespace {
void fillStats(Db::Impl* p, AircraftRecord& r);
}

bool Db::aircraft(uint32_t icao, AircraftRecord& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->db) return false;
    Stmt s(p_->db, "SELECT icao,registration,type,operator,category,country,rotorcraft,kind,kind_conf,kind_basis,kind_user,notes,tags,pictures"
                   " FROM aircraft WHERE icao = ?1");
    if (!s) return false;
    s.bindI(1, icao);
    if (!s.step()) return false;
    out = AircraftRecord();
    out.icao = (uint32_t)s.colI(0);
    out.registration = s.colS(1);
    out.type = s.colS(2);
    out.operatorName = s.colS(3);
    out.category = s.colS(4);
    out.country = s.colS(5);
    out.rotorcraft = s.colI(6) != 0;
    out.kind = (OperationKind)s.colI(7);
    out.kindConfidence = (float)s.col(8);
    out.kindBasis = s.colS(9);
    out.kindUser = s.isNull(10) ? -1 : (int)s.colI(10);
    out.notes = s.colS(11);
    out.tags = split(s.colS(12), '\n');
    out.pictures = split(s.colS(13), '\n');
    fillStats(p_.get(), out);
    return true;
}

namespace {
// Everything that can be worked out from the flights, asked for when the dossier is read so it can never disagree with them.
void fillStats(Db::Impl* p, AircraftRecord& r) {
    const std::string w = " WHERE icao = " + std::to_string(r.icao);
    Stmt s(p->db, "SELECT COUNT(*), MIN(t0), MAX(t1), SUM(airborne), SUM(distance), SUM(messages), MAX(max_alt), MAX(max_speed),"
                  " AVG(NULLIF(cruise_alt,0)), AVG(NULLIF(cruise_speed,0)) FROM flights" + w);
    if (s.step()) {
        r.flightCount = s.colI(0);
        r.firstSeen = s.col(1);
        r.lastSeen = s.col(2);
        r.airborneSec = s.col(3);
        r.distanceNm = s.col(4);
        r.messages = s.colI(5);
        r.maxAltFt = (int)s.colI(6);
        r.maxSpeedKt = (float)s.col(7);
        r.meanCruiseAltFt = (float)s.col(8);
        r.meanCruiseSpeedKt = (float)s.col(9);
    }
    // the airport it begins or ends at most often
    Stmt h(p->db, "SELECT ap, COUNT(*) c FROM (SELECT org_icao ap FROM flights" + w + " AND org_icao <> ''"
                  " UNION ALL SELECT dst_icao ap FROM flights" + w + " AND dst_icao <> '') GROUP BY ap ORDER BY c DESC LIMIT 1");
    if (h.step()) { r.homeIcao = h.colS(0); r.homeCount = h.colI(1); }
    // the mean gap on the ground between consecutive flights that both touched the same airport
    Stmt g(p->db, "SELECT t0, t1, org_icao, dst_icao FROM flights" + w + " ORDER BY t0 ASC");
    double prevEnd = 0;
    std::string prevDst;
    double sum = 0;
    int n = 0;
    while (g.step()) {
        const double t0 = g.col(0), t1 = g.col(1);
        const std::string org = g.colS(2), dst = g.colS(3);
        if (prevEnd > 0 && !prevDst.empty() && prevDst == org && t0 > prevEnd) { sum += t0 - prevEnd; n++; }
        prevEnd = t1;
        prevDst = dst;
    }
    if (n) r.meanGroundSec = sum / n;
}
} // namespace

std::vector<AircraftRecord> Db::aircraftList(const Filter& f) const {
    std::vector<uint32_t> ids;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (!p_->db) return {};
        Impl::Binder b;
        const std::string w = p_->whereBound(f, b);
        Stmt s(p_->db, "SELECT f.icao, COUNT(*) c" + std::string(Impl::kFrom()) + w + " GROUP BY f.icao ORDER BY c DESC LIMIT " +
                           std::to_string(f.limit > 0 ? f.limit : 500) + " OFFSET " + std::to_string(std::max(0, f.offset)));
        if (!s) return {};
        Impl::bindAll(s, b);
        while (s.step()) ids.push_back((uint32_t)s.colI(0));
    }
    std::vector<AircraftRecord> out;
    for (uint32_t id : ids) {
        AircraftRecord r;
        if (aircraft(id, r)) out.push_back(std::move(r));
    }
    return out;
}

std::vector<Fact> Db::facts(uint32_t icao) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Fact> out;
    if (!p_->db) return out;
    Stmt s(p_->db, "SELECT key,value,edited FROM facts WHERE icao = ?1 ORDER BY key");
    if (!s) return out;
    s.bindI(1, icao);
    while (s.step()) out.push_back({s.colS(0), s.colS(1), s.col(2)});
    return out;
}

std::vector<Event> Db::events(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Event> out;
    if (!p_->db) return out;
    std::string w;
    auto add = [&](const std::string& c) { w += w.empty() ? " WHERE " : " AND "; w += c; };
    if (f.icao) add("icao = " + std::to_string(f.icao));
    if (f.since > 0) add("t >= " + num(f.since, 3));
    if (f.until > 0) add("t <= " + num(f.until, 3));
    Stmt s(p_->db, "SELECT id,icao,flight,t,kind,txt,user FROM events" + w + " ORDER BY t " + (f.oldestFirst ? "ASC" : "DESC") +
                       " LIMIT " + std::to_string(f.limit > 0 ? f.limit : 500));
    if (!s) return out;
    while (s.step()) {
        Event e;
        e.id = s.colI(0);
        e.icao = (uint32_t)s.colI(1);
        e.flightId = s.colI(2);
        e.t = s.col(3);
        e.kind = s.colS(4);
        e.text = s.colS(5);
        e.user = s.colI(6) != 0;
        out.push_back(std::move(e));
    }
    return out;
}

// ---------------------------------------------------------------- rankings

namespace {
// Runs a grouping query that yields key (or icao), count, seconds, value and optionally a label, and returns the rows.
std::vector<Ranked> rankRows(Db::Impl* p, const std::string& sql, const Db::Impl::Binder& b, bool withIcao, bool labelCol) {
    std::vector<Ranked> out;
    if (!p->db) return out;
    Stmt s(p->db, sql);
    if (!s) return out;
    Db::Impl::bindAll(s, b);
    while (s.step()) {
        Ranked r;
        int i = 0;
        if (withIcao) { r.icao = (uint32_t)s.colI(i++); r.key = hex6(r.icao); }
        else r.key = s.colS(i++);
        r.count = s.colI(i++);
        r.seconds = s.col(i++);
        r.value = s.col(i++);
        if (labelCol) r.label = s.colS(i++);
        out.push_back(std::move(r));
    }
    return out;
}
} // namespace

std::vector<Ranked> Db::topAircraft(const Filter& f, int limit) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    // the label is the registration where one is known, else the last call sign
    const std::string sql = "SELECT f.icao, COUNT(*), SUM(f.airborne), AVG(NULLIF(f.cruise_alt,0)),"
                            " COALESCE(NULLIF(a.registration,''), NULLIF(MAX(f.callsign),''), '')" +
                            std::string(Impl::kFrom()) + w + " GROUP BY f.icao ORDER BY COUNT(*) DESC, SUM(f.airborne) DESC LIMIT " +
                            std::to_string(limit > 0 ? limit : 25);
    return rankRows(p_.get(), sql, b, true, true);
}

std::vector<Ranked> Db::topAirports(const Filter& f, int limit) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    // an airport counts once for a departure and once for an arrival
    const std::string sql =
        "SELECT ap, COUNT(*), SUM(sec), 0 FROM ("
        "SELECT f.org_icao ap, f.airborne sec" + std::string(Impl::kFrom()) + w + (w.empty() ? " WHERE " : " AND ") + "f.org_icao <> '' "
        "UNION ALL "
        "SELECT f.dst_icao ap, f.airborne sec" + std::string(Impl::kFrom()) + w + (w.empty() ? " WHERE " : " AND ") + "f.dst_icao <> ''"
        ") GROUP BY ap ORDER BY COUNT(*) DESC LIMIT " + std::to_string(limit > 0 ? limit : 25);
    // the clause appears twice, so the parameters have to be bound twice over
    Impl::Binder b2 = b;
    for (const auto& kv : b.n) b2.n.push_back(kv);
    for (const auto& kv : b.t) b2.t.push_back(kv);
    std::vector<Ranked> out;
    if (!p_->db) return out;
    Stmt s(p_->db, sql);
    if (!s) return out;
    // bind the first copy, then the same values again shifted by the number of parameters
    const int nparams = (int)(b.n.size() + b.t.size());
    for (const auto& kv : b.n) { s.bind(kv.first, kv.second); s.bind(kv.first + nparams, kv.second); }
    for (const auto& kv : b.t) { s.bind(kv.first, kv.second); s.bind(kv.first + nparams, kv.second); }
    while (s.step()) {
        Ranked r;
        r.key = s.colS(0);
        r.label = r.key;
        r.count = s.colI(1);
        r.seconds = s.col(2);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<Ranked> Db::topOperators(const Filter& f, int limit) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    const std::string sql = "SELECT f.op3, COUNT(*), SUM(f.airborne), COUNT(DISTINCT f.icao)" + std::string(Impl::kFrom()) + w +
                            (w.empty() ? " WHERE " : " AND ") + "f.op3 <> '' GROUP BY f.op3 ORDER BY COUNT(*) DESC LIMIT " +
                            std::to_string(limit > 0 ? limit : 25);
    return rankRows(p_.get(), sql, b, false, false);
}

std::vector<Ranked> Db::topTypes(const Filter& f, int limit) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    const std::string sql = "SELECT a.type, COUNT(*), SUM(f.airborne), COUNT(DISTINCT f.icao)" + std::string(Impl::kFrom()) + w +
                            (w.empty() ? " WHERE " : " AND ") + "COALESCE(a.type,'') <> '' GROUP BY a.type ORDER BY COUNT(*) DESC LIMIT " +
                            std::to_string(limit > 0 ? limit : 25);
    return rankRows(p_.get(), sql, b, false, false);
}

std::vector<Ranked> Db::topRoutes(const Filter& f, int limit) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    const std::string sql = "SELECT f.org_icao || ' - ' || f.dst_icao, COUNT(*), SUM(f.airborne), AVG(f.distance)" +
                            std::string(Impl::kFrom()) + w + (w.empty() ? " WHERE " : " AND ") +
                            "f.org_icao <> '' AND f.dst_icao <> '' GROUP BY f.org_icao, f.dst_icao ORDER BY COUNT(*) DESC LIMIT " +
                            std::to_string(limit > 0 ? limit : 25);
    return rankRows(p_.get(), sql, b, false, false);
}

std::vector<Ranked> Db::byKind(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    const std::string sql = "SELECT COALESCE(a.kind_user, f.kind) k, COUNT(*), SUM(f.airborne), COUNT(DISTINCT f.icao)" +
                            std::string(Impl::kFrom()) + w + " GROUP BY k ORDER BY COUNT(*) DESC";
    auto rows = rankRows(p_.get(), sql, b, false, false);
    for (auto& r : rows) {
        const int k = atoi(r.key.c_str());
        r.label = operationKindName((OperationKind)k);
    }
    return rows;
}

std::vector<Ranked> Db::kindAirports(const Filter& f, int kind, bool destinations, int limit) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    Filter g = f;
    g.kind = kind;
    const std::string w = p_->whereBound(g, b);
    const char* col = destinations ? "f.dst_icao" : "f.org_icao";
    const std::string sql = std::string("SELECT ") + col + ", COUNT(*), SUM(f.airborne), COUNT(DISTINCT f.icao)" + Impl::kFrom() + w +
                            (w.empty() ? " WHERE " : " AND ") + col + " <> '' GROUP BY " + col + " ORDER BY COUNT(*) DESC LIMIT " +
                            std::to_string(limit > 0 ? limit : 25);
    auto rows = rankRows(p_.get(), sql, b, false, false);
    for (auto& r : rows) r.label = r.key;
    return rows;
}

// ---------------------------------------------------------------- distributions

namespace {
std::vector<Bucket> buckets(Db::Impl* p, const std::string& sql, const Db::Impl::Binder& b) {
    std::vector<Bucket> out;
    if (!p->db) return out;
    Stmt s(p->db, sql);
    if (!s) return out;
    Db::Impl::bindAll(s, b);
    while (s.step()) out.push_back({(int)s.colI(0), s.colI(1), s.col(2)});
    return out;
}
} // namespace

std::vector<Bucket> Db::hourHistogram(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    return buckets(p_.get(), "SELECT f.hour, COUNT(*), SUM(f.airborne)" + std::string(Impl::kFrom()) + w + " GROUP BY f.hour ORDER BY f.hour", b);
}

std::vector<Bucket> Db::weekdayHistogram(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    return buckets(p_.get(), "SELECT f.dow, COUNT(*), SUM(f.airborne)" + std::string(Impl::kFrom()) + w + " GROUP BY f.dow ORDER BY f.dow", b);
}

std::vector<Bucket> Db::altitudeHistogram(const Filter& f, int bandFt) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    const int band = bandFt > 0 ? bandFt : 2000;
    return buckets(p_.get(), "SELECT (f.cruise_alt / " + std::to_string(band) + ") * " + std::to_string(band) +
                                 ", COUNT(*), AVG(f.cruise_speed)" + std::string(Impl::kFrom()) + w +
                                 (w.empty() ? " WHERE " : " AND ") + "f.cruise_alt > 0 GROUP BY 1 ORDER BY 1", b);
}

std::vector<Bucket> Db::speedHistogram(const Filter& f, int bandKt) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    const int band = bandKt > 0 ? bandKt : 25;
    return buckets(p_.get(), "SELECT (CAST(f.cruise_speed AS INTEGER) / " + std::to_string(band) + ") * " + std::to_string(band) +
                                 ", COUNT(*), AVG(f.cruise_alt)" + std::string(Impl::kFrom()) + w +
                                 (w.empty() ? " WHERE " : " AND ") + "f.cruise_speed > 0 GROUP BY 1 ORDER BY 1", b);
}

std::vector<Bucket> Db::dailyCounts(const Filter& f) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    return buckets(p_.get(), "SELECT f.day, COUNT(*), SUM(f.airborne)" + std::string(Impl::kFrom()) + w + " GROUP BY f.day ORDER BY f.day", b);
}

// ---------------------------------------------------------------- patterns

Regularity Db::regularity(uint32_t icao) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    Regularity r;
    if (!p_->db) return r;
    const std::string w = " WHERE icao = " + std::to_string(icao) + " AND route <> ''";
    r.flights = p_->scalar(("SELECT COUNT(*) FROM flights" + w).c_str());
    r.distinctRoutes = p_->scalar(("SELECT COUNT(DISTINCT route) FROM flights" + w).c_str());
    Stmt s(p_->db, "SELECT route, COUNT(*) c FROM flights" + w + " GROUP BY route ORDER BY c DESC LIMIT 1");
    if (s.step()) { r.commonest = s.colS(0); r.commonestCount = s.colI(1); }
    if (r.flights) r.share = (float)r.commonestCount / (float)r.flights;
    return r;
}

std::vector<Anomaly> Db::anomalies(const Filter& f, int limit) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Anomaly> out;
    if (!p_->db) return out;
    Impl::Binder b;
    const std::string w = p_->whereBound(f, b);
    // Everything an aircraft's own history makes unusual: a route it has not flown before (and it has flown enough for that to mean
    // something), an hour it is not normally up at, an emergency, or a flight far outside its own altitude or distance range.
    const std::string sql =
        "SELECT f.id, f.icao, f.t0, COALESCE(a.registration,''), f.callsign, f.route, f.hour, f.cruise_alt, f.distance,"
        " f.emergency, f.emerg_squawk,"
        " (SELECT COUNT(*) FROM flights g WHERE g.icao = f.icao AND g.id <> f.id) prior,"
        " (SELECT COUNT(*) FROM flights g WHERE g.icao = f.icao AND g.id <> f.id AND g.route = f.route) sameRoute,"
        " (SELECT COUNT(*) FROM flights g WHERE g.icao = f.icao AND g.id <> f.id AND g.hour = f.hour) sameHour,"
        " (SELECT AVG(g.cruise_alt) FROM flights g WHERE g.icao = f.icao AND g.id <> f.id AND g.cruise_alt > 0) avgAlt,"
        " (SELECT AVG(g.distance) FROM flights g WHERE g.icao = f.icao AND g.id <> f.id AND g.distance > 0) avgDist" +
        std::string(Impl::kFrom()) + w + " ORDER BY f.t0 DESC LIMIT 4000";
    Stmt s(p_->db, sql);
    if (!s) return out;
    Impl::bindAll(s, b);
    while (s.step()) {
        Anomaly a;
        a.flightId = s.colI(0);
        a.icao = (uint32_t)s.colI(1);
        a.t = s.col(2);
        a.registration = s.colS(3);
        a.callsign = s.colS(4);
        const std::string route = s.colS(5);
        const int hour = (int)s.colI(6);
        const int alt = (int)s.colI(7);
        const double dist = s.col(8);
        const int emerg = (int)s.colI(9);
        const bool esq = s.colI(10) != 0;
        const int64_t prior = s.colI(11), sameRoute = s.colI(12), sameHour = s.colI(13);
        const double avgAlt = s.col(14), avgDist = s.col(15);
        std::string why;
        float score = 0;
        if (emerg > 0 || esq) { score += 1.0f; why = esq ? "emergency squawk" : "emergency state"; }
        if (prior >= 5 && sameRoute == 0 && !route.empty()) { score += 0.6f; why += (why.empty() ? "" : "; ") + std::string("a path it has not flown before"); }
        if (prior >= 10 && sameHour == 0) { score += 0.3f; why += (why.empty() ? "" : "; ") + std::string("an hour it is not usually up at"); }
        if (prior >= 5 && avgAlt > 1000 && alt > 0 && (alt > avgAlt * 1.8 || alt < avgAlt * 0.5)) {
            score += 0.3f;
            why += (why.empty() ? "" : "; ") + std::string(alt > avgAlt ? "much higher than it usually flies" : "much lower than it usually flies");
        }
        if (prior >= 5 && avgDist > 5 && dist > avgDist * 2.5) { score += 0.2f; why += (why.empty() ? "" : "; ") + std::string("much further than usual"); }
        if (score <= 0) continue;
        a.score = score;
        a.reason = why;
        out.push_back(std::move(a));
    }
    std::sort(out.begin(), out.end(), [](const Anomaly& x, const Anomaly& y) { return x.score > y.score || (x.score == y.score && x.t > y.t); });
    if ((int)out.size() > (limit > 0 ? limit : 50)) out.resize((size_t)(limit > 0 ? limit : 50));
    return out;
}

Ranked Db::rateVsUsual(const Filter& f, double windowSec, int backWeeks) const {
    Ranked r;
    const double now = f.until > 0 ? f.until : (double)std::time(nullptr);
    if (windowSec <= 0) windowSec = 86400;
    if (backWeeks < 1) backWeeks = 1;
    Filter w = f;
    w.until = now;
    w.since = now - windowSec;
    r.count = countFlights(w);
    // The weeks before, for comparison. A week that falls entirely before anything was ever recorded is not a quiet week: there was no
    // receiver then, and counting it as a zero would make an ordinary day look busier than usual.
    const double oldest = stats().oldest;
    double sum = 0;
    int n = 0;
    for (int i = 1; i <= backWeeks; i++) {
        Filter g = f;
        g.until = now - i * 604800.0;
        g.since = g.until - windowSec;
        if (oldest > 0 && g.until < oldest) continue;
        sum += (double)countFlights(g);
        n++;
    }
    const double mean = n ? sum / n : 0;
    r.value = mean > 0 ? (double)r.count / mean : (r.count > 0 ? 0.0 : 1.0);
    r.seconds = mean;
    r.key = "rate";
    char b[96];
    snprintf(b, sizeof b, "%lld in the window, %.1f usual", (long long)r.count, mean);
    r.label = b;
    return r;
}

std::vector<double> Db::groundGaps(uint32_t icao) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<double> out;
    if (!p_->db) return out;
    Stmt g(p_->db, "SELECT t0,t1,org_icao,dst_icao FROM flights WHERE icao = ?1 ORDER BY t0 ASC");
    if (!g) return out;
    g.bindI(1, icao);
    double prevEnd = 0;
    std::string prevDst;
    while (g.step()) {
        const double t0 = g.col(0), t1 = g.col(1);
        const std::string org = g.colS(2), dst = g.colS(3);
        if (prevEnd > 0 && !prevDst.empty() && prevDst == org && t0 > prevEnd) out.push_back(t0 - prevEnd);
        prevEnd = t1;
        prevDst = dst;
    }
    return out;
}

// ---------------------------------------------------------------- export

std::string Db::exportFlights(const Filter& f, const std::string& format) const {
    Filter g = f;
    g.limit = 1000000;
    const auto v = flights(g);
    std::string o;
    if (format == "json") {
        o = "[";
        for (size_t i = 0; i < v.size(); i++) {
            const Flight& x = v[i];
            if (i) o += ",";
            o += "\n {\"icao\":\"" + hex6(x.icao) + "\",\"callsign\":\"" + jsonEsc(x.callsign) + "\",\"t0\":" + num(x.tStart, 3) +
                 ",\"t1\":" + num(x.tEnd, 3) + ",\"airborne\":" + num(x.airborneSec, 1) +
                 ",\"origin\":\"" + jsonEsc(x.origin.icao) + "\",\"destination\":\"" + jsonEsc(x.destination.icao) +
                 "\",\"max_alt_ft\":" + std::to_string(x.maxAltFt) + ",\"cruise_alt_ft\":" + std::to_string(x.cruiseAltFt) +
                 ",\"cruise_speed_kt\":" + num(x.cruiseSpeedKt, 0) + ",\"distance_nm\":" + num(x.distanceNm, 1) +
                 ",\"kind\":\"" + operationKindName((OperationKind)x.kind) + "\",\"squawk\":\"" + jsonEsc(x.squawk) + "\"}";
        }
        o += "\n]\n";
        return o;
    }
    o = "icao,callsign,start,end,airborne_s,origin,origin_rwy,destination,destination_rwy,max_alt_ft,cruise_alt_ft,cruise_speed_kt,"
        "distance_nm,squawk,emergency,kind,kind_confidence\r\n";
    for (const Flight& x : v) {
        o += hex6(x.icao) + "," + csvEsc(x.callsign) + "," + num(x.tStart, 3) + "," + num(x.tEnd, 3) + "," + num(x.airborneSec, 1) + "," +
             csvEsc(x.origin.icao) + "," + csvEsc(x.origin.runway) + "," + csvEsc(x.destination.icao) + "," + csvEsc(x.destination.runway) + "," +
             std::to_string(x.maxAltFt) + "," + std::to_string(x.cruiseAltFt) + "," + num(x.cruiseSpeedKt, 0) + "," + num(x.distanceNm, 1) + "," +
             csvEsc(x.squawk) + "," + std::to_string(x.emergency) + "," + operationKindName((OperationKind)x.kind) + "," +
             num(x.kindConfidence, 2) + "\r\n";
    }
    return o;
}

std::string Db::exportTrack(int64_t flightId, const std::string& format) const {
    const auto pts = track(flightId);
    Flight f;
    flight(flightId, f);
    std::string o;
    if (format == "gpx") {
        o = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gpx version=\"1.1\" creator=\"Eye of ZorCom\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n";
        o += " <trk><name>" + jsonEsc(f.callsign.empty() ? hex6(f.icao) : f.callsign) + "</name><trkseg>\n";
        for (const Point& p : pts) {
            o += "  <trkpt lat=\"" + num(p.lat, 6) + "\" lon=\"" + num(p.lon, 6) + "\">";
            if (p.hasAlt) o += "<ele>" + num(p.altFt * 0.3048, 1) + "</ele>";
            o += "</trkpt>\n";
        }
        o += " </trkseg></trk>\n</gpx>\n";
        return o;
    }
    if (format == "json") {
        o = "[";
        for (size_t i = 0; i < pts.size(); i++) {
            const Point& p = pts[i];
            if (i) o += ",";
            o += "\n {\"t\":" + num(p.t, 3) + ",\"lat\":" + num(p.lat, 6) + ",\"lon\":" + num(p.lon, 6);
            if (p.hasAlt) o += ",\"alt_ft\":" + std::to_string(p.altFt);
            if (p.hasSpeed) o += ",\"speed_kt\":" + num(p.speedKt, 0);
            o += ",\"phase\":\"" + std::string(phaseName(p.phase)) + "\"}";
        }
        o += "\n]\n";
        return o;
    }
    o = "time,lat,lon,alt_ft,speed_kt,track_deg,vrate_fpm,ground,phase\r\n";
    for (const Point& p : pts) {
        o += num(p.t, 3) + "," + num(p.lat, 6) + "," + num(p.lon, 6) + "," + (p.hasAlt ? std::to_string(p.altFt) : "") + "," +
             (p.hasSpeed ? num(p.speedKt, 0) : "") + "," + (p.hasTrack ? num(p.trackDeg, 1) : "") + "," +
             (p.hasVrate ? std::to_string(p.vrateFpm) : "") + "," + (p.ground ? "1" : "0") + "," + phaseName(p.phase) + "\r\n";
    }
    return o;
}

Db& global() {
    static Db db;
    return db;
}

} // namespace dect2::flights
