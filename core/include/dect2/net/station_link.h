// StationLinks: the collector of the station network. For each configured site it keeps a connection to that station's remote API
// (GET /api/v1/ws, {"events":true[,"since":N]}), tags every received event with f["site"] = the site's name and submits it to the local automation
// bus (automation::emit), from where the Eye, the message database and the watchlists pick it up. One thread per site; a lost connection is retried
// after 2, 4, 8 ... 60 s (back to 2 s once a connection has lived 30 s). A reconnect sends since = the last seq seen, so nothing is read twice.
// The remote station's position is read from GET /api/v1/status ("lat" / "lon", NaN when it does not send them) once per connect.
// Keepalive: the link sends a WebSocket ping every LinkTiming::pingMs (15 s) and drops the connection (then retries as above) when nothing at all,
// not even a pong, has come for LinkTiming::idleMs (45 s): a station that lost power or its network is noticed instead of staying "live".
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
    double clockSkewSec = 0;      // smoothed (receive time - event time): the remote clock's lag, EWMA 0.1; events with time 0 are ignored, and so is
                                  // the catch-up after a (re)subscription: in its first 10 s, events more than 10 s older than the current estimate
    double lat = NAN, lon = NAN;
};

// The keepalive of every link (the defaults are the real ones; tests use short ones)
struct LinkTiming {
    int pingMs = 15000;   // a ping this often
    int idleMs = 45000;   // nothing received for this long: the connection is closed and retried ("no data from the station for 45 s")
};

class StationLinks {
public:
    explicit StationLinks(LinkTiming t = LinkTiming());
    ~StationLinks();
    StationLinks(const StationLinks&) = delete;
    StationLinks& operator=(const StationLinks&) = delete;
    // Makes the running links match the list: unchanged sites keep running, changed or removed ones are told to stop (their socket is shut down, which
    // wakes a reading thread at once) and left to finish on their own: configure() never joins, so it is safe to call from the UI thread), new ones are started. Sites with an empty name or host are ignored; a later site with a name already used is ignored.
    void configure(const std::vector<SiteConfig>& sites);
    // Config and status of every site, in the order configured
    std::vector<std::pair<SiteConfig, SiteStatus>> status() const;
    // Stops every link, forgets the sites and joins all threads. May block for one connect or name-lookup timeout (a thread inside connect() or
    // getaddrinfo() cannot be woken); call it from shutdown, not from the UI.
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

StationLinks& globalLinks();

// The saved sites (<dataDir>/stations/sites.json: {"sites":[{name,host,port,token,on}]}). The tokens are secrets, so saveSites writes the file with mode
// 0600. loadSites: a missing file is an empty list and true; a corrupt one is an empty list, false and err. Entries without a name or host are skipped, as are later ones with a name already used,
// a port outside 1..65535 becomes 8765. Paths are UTF-8.
bool loadSites(const std::string& path, std::vector<SiteConfig>& out, std::string& err);
bool saveSites(const std::string& path, const std::vector<SiteConfig>& sites, std::string& err);

}  // namespace dect2
