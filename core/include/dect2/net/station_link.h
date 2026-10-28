// StationLinks: the collector of the station network. For each configured site it keeps a connection to that station's remote API
// (GET /api/v1/ws, {"events":true[,"since":N]}), tags every received event with f["site"] = the site's name and submits it to the local automation
// bus (automation::emit), from where the Eye, the message database and the watchlists pick it up. One thread per site; a lost connection is retried
// after 2, 4, 8 ... 60 s (back to 2 s once a connection has lived 30 s). A reconnect sends since = the last seq seen, so nothing is read twice.
// The remote station's position is read from GET /api/v1/status ("lat" / "lon", NaN when it does not send them) once per connect.
#pragma once
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct SiteConfig {
    std::string name, host;
    int port = 8765;
    std::string token;      // sent as ?token= (a LAN peer needs it; loopback does not)
    bool on = true;
};

struct SiteStatus {
    int state = 0;          // 0 off, 1 connecting, 2 live, 3 retrying
    std::string lastError;
    uint64_t events = 0, missed = 0;
    double lastEventAt = 0;       // Unix seconds (local clock) when the last event came
    double clockSkewSec = 0;      // smoothed (receive time - event time): the remote clock's lag, EWMA 0.1; events with time 0 are ignored
    double lat = NAN, lon = NAN;
};

class StationLinks {
public:
    StationLinks();
    ~StationLinks();
    StationLinks(const StationLinks&) = delete;
    StationLinks& operator=(const StationLinks&) = delete;
    // Makes the running links match the list: unchanged sites keep running, changed or removed ones are stopped (their socket is closed, which wakes the
    // thread at once), new ones are started. Sites with an empty name or host are ignored; a later site with a name already used is ignored.
    void configure(const std::vector<SiteConfig>& sites);
    // Config and status of every site, in the order configured
    std::vector<std::pair<SiteConfig, SiteStatus>> status() const;
    void stop();   // stops every link and forgets the sites
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

StationLinks& globalLinks();

}  // namespace dect2
