// zorcom-server: Eye of ZorCom without a window. The radio is attached to this computer (a Raspberry Pi, a Linux box, a Windows PC with no screen); it is
// controlled from a web browser (the page this program serves) or by any program that speaks the remote-control API (docs/headless-server.md).
// No cloud and no relay: the browser connects straight to this program. Access from other computers needs a token; TLS is optional.
#include "dect2/common/platform.h"
#include "dect2/engine/crash_report.h"
#include "dect2/net/remote_api.h"
#include "dect2/net/share.h"
#include "server_config.h"
#include "server_host.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace zorcom_server;

static std::atomic<bool> gQuit{false};
static void onSignal(int) { gQuit = true; }

static void usage() {
    printf("zorcom-server %s: Eye of ZorCom with no window, controlled from a web browser or the remote-control API\n\n"
           "  zorcom-server [--config FILE] [options]\n\n"
           "  --config FILE      settings file (default %s)\n"
           "  --port N           web / API port (default 8765)\n"
           "  --lan              accept other computers (needs a token; made and printed when there is none)\n"
           "  --token TEXT       use this access token (16 characters or more) for this run\n"
           "  --new-token        make a new token, save it in the settings file and print it\n"
           "  --tls-cert FILE --tls-key FILE   serve HTTPS (PEM files; needs a build with OpenSSL, DECT2_WITH_TLS)\n"
           "  --device NAME|N    radio to use (see the device list in the page); 'test' is the built-in test signal\n"
           "  --mode ID          receiver mode, e.g. fm, dab, t2, adsb, dvbs (see /api/v1/modes)\n"
           "  --freq MHZ         tuning frequency\n"
           "  --autostart        start the receiver at once and keep it running (comes back when the radio is replugged)\n"
           "  --share-rtltcp [PORT]   also share the raw samples as an rtl_tcp server (default port 1234) for SDR++, SDR# or another Eye of ZorCom; no access control: trusted networks only\n"
           "  --version, --help\n\n"
           "Settings file: plain key=value lines (see docs/headless-server.md).\n",
           ZORCOM_SERVER_VERSION, defaultConfigPath().c_str());
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // a service log (journald, a file) shows lines as they happen
    dect2::crash::install("crash-server");
    if (!dect2::cpuSupportsBuild()) { fprintf(stderr, "this build needs a processor with AVX2 and FMA\n"); return 1; }
    std::string path = defaultConfigPath();
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h") { usage(); return 0; }
        if (a == "--version") { printf("zorcom-server %s\n", ZORCOM_SERVER_VERSION); return 0; }
        if (a == "--config" && i + 1 < argc) path = argv[++i];
    }
    ConfigFile cfg;
    const bool haveFile = cfg.load(path);

    dect2::RemoteApiConfig api;
    api.on = true;
    api.port = (int)cfg.num("port", 8765);
    api.lan = cfg.flag("lan", false);
    api.token = cfg.get("token");
    api.tlsCert = cfg.get("tls_cert");
    api.tlsKey = cfg.get("tls_key");
    bool newToken = false, share = cfg.flag("share_rtltcp", false);
    int sharePort = (int)cfg.num("share_port", 1234);
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--config") next();
        else if (a == "--port") api.port = atoi(next().c_str());
        else if (a == "--lan") api.lan = true;
        else if (a == "--token") api.token = next();
        else if (a == "--new-token") newToken = true;
        else if (a == "--tls-cert") api.tlsCert = next();
        else if (a == "--tls-key") api.tlsKey = next();
        else if (a == "--device") cfg.set("device", next());
        else if (a == "--mode") cfg.set("mode", next());
        else if (a == "--freq") cfg.set("freq_mhz", next());
        else if (a == "--autostart") cfg.set("autostart", "1");
        else if (a == "--share-rtltcp") { share = true; if (i + 1 < argc && argv[i + 1][0] != '-') sharePort = atoi(argv[++i]); }
        else { fprintf(stderr, "unknown option %s (try --help)\n", a.c_str()); return 2; }
    }
    if (api.port < 1 || api.port > 65535) { fprintf(stderr, "the port must be 1 to 65535\n"); return 2; }
    if (api.tlsCert.empty() != api.tlsKey.empty()) { fprintf(stderr, "TLS needs both tls_cert and tls_key\n"); return 2; }

    bool saveCfg = !haveFile;   // a first run leaves a file to edit
    if (newToken || (api.lan && api.token.size() < 16)) {
        api.token = dect2::remoteApiNewToken();
        cfg.set("token", api.token);
        saveCfg = true;
        fprintf(stderr, "zorcom-server: new access token %s (saved in %s)\n", api.token.c_str(), path.c_str());
    }
    if (api.token.size() > 0 && api.token.size() < 16) { fprintf(stderr, "zorcom-server: the token must have 16 characters or more\n"); return 2; }
    if (saveCfg) {
        std::string err;
        if (!cfg.save(path, err)) fprintf(stderr, "zorcom-server: %s (the settings will not be saved)\n", err.c_str());
    }

    ServerHost host(cfg);
    host.init();
    dect2::RemoteApiServer server(host);
    if (!server.configure(api)) {
        fprintf(stderr, "zorcom-server: %s\n", server.status().lastError.c_str());
        return 1;
    }
    const dect2::RemoteApiConfig live = server.config();
    const char* scheme = server.status().tls ? "https" : "http";
    if (api.lan) {
        printf("zorcom-server: listening on all interfaces, port %d. Open %s://<this computer>:%d/?token=<token from %s>\n", live.port, scheme, live.port, path.c_str());
        if (!server.status().tls) fprintf(stderr, "zorcom-server: no TLS: the token and the page travel unencrypted. Use it on a network you trust, or set tls_cert / tls_key (or a reverse proxy).\n");
    } else {
        printf("zorcom-server: listening on this computer only: %s://127.0.0.1:%d/ (use --lan to let other computers in)\n", scheme, live.port);
    }
    if (share) {
        dect2::ShareConfig sc;
        sc.on = true; sc.port = sharePort; sc.lan = api.lan; sc.allowControl = false;
        dect2::ShareServer::get().configure(sc);
        const dect2::ShareStatus ss = dect2::ShareServer::get().status();
        if (!ss.listening) fprintf(stderr, "zorcom-server: rtl_tcp share: %s\n", ss.lastError.c_str());
        else printf("zorcom-server: sharing the samples as rtl_tcp on port %d (%s; no password)\n", sharePort, api.lan ? "whole network" : "this computer only");
    }
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif
    dect2::crash::startupDone();
    while (!gQuit) {
        host.supervise(path);
        for (int i = 0; i < 10 && !gQuit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("zorcom-server: stopping\n");
    host.supervise(path);
    host.shutdown();   // the receiver stops; the saved "autostart" keeps what the user last chose, so a restart brings it back
    dect2::ShareServer::get().configure(dect2::ShareConfig{});
    return 0;
}
