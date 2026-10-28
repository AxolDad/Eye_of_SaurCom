// sites.json of the station network: save/load round trip, file mode 0600 (it holds tokens), a corrupt file loads as empty with an error.
#include "../common/test_util.h"
#include "dect2/net/station_link.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif
using namespace dect2;
namespace fs = std::filesystem;

int main() {
    const fs::path dir = fs::temp_directory_path() / "dect2_station_sites_test";
    fs::remove_all(dir);
    const std::string path = (dir / "stations" / "sites.json").string();

    std::vector<SiteConfig> in(2);
    in[0].name = "Roof"; in[0].host = "192.168.1.20"; in[0].port = 8765; in[0].token = "0123456789abcdef\"x\\y"; in[0].on = true;
    in[1].name = "Cabin \xC3\xA4"; in[1].host = "cabin.local"; in[1].port = 9000; in[1].token = ""; in[1].on = false;

    std::string err;
    CHECK(saveSites(path, in, err), "save: %s", err.c_str());
    std::vector<SiteConfig> out;
    CHECK(loadSites(path, out, err), "load: %s", err.c_str());
    CHECK(out.size() == in.size(), "count %zu", out.size());
    for (size_t i = 0; i < in.size() && i < out.size(); i++) {
        CHECK(out[i].name == in[i].name && out[i].host == in[i].host && out[i].port == in[i].port && out[i].token == in[i].token && out[i].on == in[i].on, "site %zu differs", i);
    }
#ifndef _WIN32
    struct stat st{};
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "mode %o", (unsigned)(st.st_mode & 0777));
#endif

    // a missing file is an empty list and not an error
    out.assign(1, SiteConfig{});
    CHECK(loadSites((dir / "none.json").string(), out, err) && out.empty(), "missing file");

    // a corrupt file: empty list, an error text
    { std::ofstream o(path, std::ios::binary | std::ios::trunc); o << "{ not json ["; }
    out.assign(1, SiteConfig{});
    err.clear();
    CHECK(!loadSites(path, out, err) && out.empty() && !err.empty(), "corrupt file: ok/out/err");

    // wrong shape and bad values
    { std::ofstream o(path, std::ios::binary | std::ios::trunc); o << "{\"sites\":[{\"name\":\"A\",\"host\":\"h\",\"port\":99999},{\"host\":\"x\"},5]}"; }
    err.clear();
    CHECK(loadSites(path, out, err), "shape: %s", err.c_str());
    CHECK(out.size() == 1 && out[0].name == "A" && out[0].port == 8765, "bad port falls back, nameless skipped (%zu)", out.size());

    // a repeated name: the first one is kept
    { std::ofstream o(path, std::ios::binary | std::ios::trunc); o << "{\"sites\":[{\"name\":\"a\",\"host\":\"h1\"},{\"name\":\"a\",\"host\":\"h2\"},{\"name\":\"b\",\"host\":\"h3\"}]}"; }
    CHECK(loadSites(path, out, err) && out.size() == 2 && out[0].host == "h1" && out[1].name == "b", "duplicate names (%zu)", out.size());

    // a second save over an existing file: still private, no .tmp left behind
#ifndef _WIN32
    chmod(path.c_str(), 0644);
#endif
    CHECK(saveSites(path, in, err), "second save: %s", err.c_str());
#ifndef _WIN32
    CHECK(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "mode after overwrite %o", (unsigned)(st.st_mode & 0777));
#endif
    CHECK(!fs::exists(path + ".tmp"), "tmp file left behind");

    fs::remove_all(dir);
    return testSummary("station_sites");
}
