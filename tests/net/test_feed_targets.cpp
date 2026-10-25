// Outbound feed targets: Beast over TCP and AIS over UDP to loopback servers, fan-out to two targets, off by default,
// reconnect after the server goes away. And the feeder id the readsb-based networks tell feeders apart by.
#include "dect2/net/feeds.h"
#include "../../core/src/net/tcp_client.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <thread>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static sock_t listenOn(int& port) {
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 4) != 0) return kBadSock;
    socklen_t l = sizeof a; getsockname(ls, (sockaddr*)&a, &l); port = ntohs(a.sin_port);
    sockSetNonBlocking(ls, true);
    return ls;
}
static sock_t acceptWait(sock_t ls, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        sock_t c = accept(ls, nullptr, nullptr);
        if (c != kBadSock) { sockRecvTimeout(c, 200); return c; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return kBadSock;
}
static std::string readSome(sock_t c, size_t want, int ms) {
    std::string o; char b[512];
    for (int i = 0; i < ms / 20 && o.size() < want; i++) {
        long long k = recv(c, b, sizeof b, 0);
        if (k > 0) o.append(b, (size_t)k); else if (k == 0) break;
        else std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return o;
}
static FeedAdsbFrame frame() {
    FeedAdsbFrame f;
    const uint8_t b[14] = {0x8D, 0x48, 0x40, 0xD6, 0x20, 0x2C, 0xC3, 0x71, 0xC3, 0x2C, 0xE0, 0x57, 0x60, 0x98};
    memcpy(f.bytes, b, 14); f.bits = 112; f.timeSec = 1; f.levelDbfs = -20;
    return f;
}
// What readsb does with a receiver-id frame, transcribed from its source (airplanes-live/readsb net_io.c: the 0xE4 case of the Beast
// reader, then read_uuid). It waits for 36 bytes after the 0xE4, then reads up to 128 characters collecting 32 hex digits, skipping '-'
// and ' ', and stops at 0x1A or any other character; 16 digits or more make an id. Kept here as an oracle so that what this program
// sends is checked against what the network actually accepts, not against our own idea of it.
static bool readsbAcceptsUuidFrame(const std::string& f, uint64_t& hi, uint64_t& lo) {
    hi = lo = 0;
    if (f.size() < 2 || (uint8_t)f[0] != 0x1A || (uint8_t)f[1] != 0xE4) return false;
    if (f.size() - 2 < 36) return false;   // "incomplete uuid": readsb waits for more
    int j = 0;
    for (size_t i = 2; i < f.size() && i < 2 + 128 && j < 32; i++) {
        const unsigned char ch = (unsigned char)f[i];
        if (ch == 0x1A) break;
        if (ch == '-' || ch == ' ') continue;
        int x;
        if (ch >= 'a' && ch <= 'f') x = ch - 'a' + 10;
        else if (ch >= '0' && ch <= '9') x = ch - '0';
        else if (ch >= 'A' && ch <= 'F') x = ch - 'A' + 10;
        else break;
        if (j < 16) hi = hi << 4 | (uint64_t)x;
        else lo = lo << 4 | (uint64_t)x;
        j++;
    }
    return j >= 16;
}

static uint64_t hexOf(const std::string& s) { return std::stoull(s, nullptr, 16); }

template <class F> static bool waitFor(F f, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (f()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    return false;
}

int main() {
    // ---- the feeder id, with no network
    {
        CHECK(feedUuidValid("123e4567-e89b-42d3-a456-426614174000"), "a canonical UUID was refused");
        CHECK(feedUuidValid("123E4567-E89B-42D3-A456-426614174000"), "an upper-case UUID was refused");
        CHECK(!feedUuidValid(""), "an empty id was accepted");
        CHECK(!feedUuidValid("123e4567e89b42d3a456426614174000"), "a UUID without hyphens was accepted");
        CHECK(!feedUuidValid("123e4567-e89b-42d3-a456-42661417400"), "a UUID one short was accepted");
        CHECK(!feedUuidValid("123e4567-e89b-42d3-a456-4266141740000"), "a UUID one long was accepted");
        CHECK(!feedUuidValid("123e4567-e89b-42d3-a456_426614174000"), "a misplaced separator was accepted");
        CHECK(!feedUuidValid("g23e4567-e89b-42d3-a456-426614174000"), "a non-hex digit was accepted");
        CHECK(!feedUuidValid("123e4567-e89b-42d3-a456-42661417400\x1a"), "a 0x1A inside was accepted");

        // A new one is a valid version 4 UUID, and two stations do not draw the same one.
        std::set<std::string> seen;
        for (int i = 0; i < 2000; i++) {
            const std::string u = feedUuidNew();
            if (!feedUuidValid(u)) { CHECK(false, "feedUuidNew made '%s'", u.c_str()); break; }
            if (u[14] != '4') { CHECK(false, "'%s' is not version 4", u.c_str()); break; }
            if (u[19] != '8' && u[19] != '9' && u[19] != 'a' && u[19] != 'b') { CHECK(false, "'%s' has the wrong variant", u.c_str()); break; }
            seen.insert(u);
        }
        CHECK(seen.size() == 2000, "2000 new ids had %zu distinct", seen.size());

        // The frame: 0x1A 0xE4 then the 36 characters, 38 bytes in all, exactly as readsb sends it.
        const std::string id = "123e4567-e89b-42d3-a456-426614174000";
        const std::string fr = feedfmt::beastUuidFrame(id);
        CHECK(fr.size() == 38, "the frame is %zu bytes, not 38", fr.size());
        CHECK(fr.size() == 38 && (uint8_t)fr[0] == 0x1A && (uint8_t)fr[1] == 0xE4, "the frame does not open 0x1A 0xE4");
        CHECK(fr.size() == 38 && fr.compare(2, 36, id) == 0, "the frame does not carry the id");
        CHECK(feedfmt::beastUuidFrame("").empty(), "an empty id made a frame");
        CHECK(feedfmt::beastUuidFrame("not-a-uuid").empty(), "a bad id made a frame");

        // And what the network makes of it: readsb's own rule, applied to our bytes, must recover the same 128 bits.
        uint64_t hi = 0, lo = 0;
        CHECK(readsbAcceptsUuidFrame(fr, hi, lo), "readsb's rule refuses the frame");
        CHECK(hi == hexOf("123e4567e89b42d3"), "readsb would read the first half as %016llx", (unsigned long long)hi);
        CHECK(lo == hexOf("a456426614174000"), "readsb would read the second half as %016llx", (unsigned long long)lo);
        for (int i = 0; i < 50; i++) {   // every generated id survives the same trip
            const std::string u = feedUuidNew();
            std::string hex;
            for (char ch : u) if (ch != '-') hex += ch;
            uint64_t h2 = 0, l2 = 0;
            if (!readsbAcceptsUuidFrame(feedfmt::beastUuidFrame(u), h2, l2) || h2 != hexOf(hex.substr(0, 16)) || l2 != hexOf(hex.substr(16))) {
                CHECK(false, "readsb would not read '%s' back as itself", u.c_str());
                break;
            }
        }
        // The oracle itself must refuse what readsb refuses, or it proves nothing.
        CHECK(!readsbAcceptsUuidFrame(std::string("\x1a\xe4") + "123e4567", hi, lo), "the oracle accepted a frame readsb would wait on");
        CHECK(!readsbAcceptsUuidFrame(std::string("\x1a\xe4") + "zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz", hi, lo), "the oracle accepted no hex at all");

        // The presets that want an id are the readsb-based networks, and they are Beast over TCP: an id is never asked for anything else.
        int wanting = 0;
        for (const FeedPreset& p : feedPresets()) {
            if (!p.wantsUuid) continue;
            wanting++;
            CHECK(p.kind == FeedKind::AdsbBeast && p.proto == FeedProto::Tcp, "%s wants an id but is not Beast over TCP", p.name);
        }
        CHECK(wanting >= 1, "no preset asks for a feeder id");
    }

    netInit();
    int p1 = 0, p2 = 0;
    sock_t l1 = listenOn(p1), l2 = listenOn(p2);
    sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in ua{}; ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(u, (sockaddr*)&ua, sizeof ua);
    socklen_t ul = sizeof ua; getsockname(u, (sockaddr*)&ua, &ul);
    sockRecvTimeout(u, 300);
    CHECK(l1 != kBadSock && l2 != kBadSock, "loopback listeners");

    FeedConfig c;
    FeedTarget a; a.name = "A"; a.kind = FeedKind::AdsbBeast; a.host = "127.0.0.1"; a.port = p1; a.key = "mykey";
    FeedTarget b = a; b.name = "B"; b.port = p2; b.key.clear();
    const std::string bId = "0f1e2d3c-4b5a-4968-8796-a5b4c3d2e1f0";
    b.uuid = bId;
    FeedTarget ai; ai.name = "AIS"; ai.kind = FeedKind::AisNmea; ai.proto = FeedProto::Udp; ai.host = "127.0.0.1"; ai.port = ntohs(ua.sin_port);
    int p3 = 0;
    sock_t l3 = listenOn(p3);
    FeedTarget sb; sb.name = "SBS"; sb.kind = FeedKind::AdsbSbs; sb.host = "127.0.0.1"; sb.port = p3;
    sb.uuid = bId;   // set by hand: the window never offers one for SBS, but a stored setting could carry one
    c.targets = {a, b, ai, sb};   // all disabled
    Feeds::get().configure(c);
    CHECK(!Feeds::get().wantAdsb() && !Feeds::get().wantAis(), "nothing wanted while every target is off");
    Feeds::get().adsb(frame());
    CHECK(acceptWait(l1, 300) == kBadSock, "no connection while disabled");

    c.targets[0].enabled = c.targets[1].enabled = c.targets[2].enabled = c.targets[3].enabled = true;
    Feeds::get().configure(c);
    CHECK(Feeds::get().wantAdsb() && Feeds::get().wantAis(), "wanted once enabled");
    sock_t c1 = acceptWait(l1, 3000), c2 = acceptWait(l2, 3000);
    CHECK(c1 != kBadSock && c2 != kBadSock, "both TCP targets connect");
    if (c1 == kBadSock || c2 == kBadSock) { printf("feed_targets: %d failures\n", ++fails); return 1; }
    CHECK(readSome(c1, 6, 1000) == "mykey\n", "key line sent first");
    // The target with an id opens with the receiver-id frame, before any data, as readsb's own feeder does.
    const std::string bOpen = readSome(c2, 38, 1000);
    CHECK(bOpen == feedfmt::beastUuidFrame(bId), "target B did not open with its feeder id (%zu bytes)", bOpen.size());
    uint64_t bh = 0, bl = 0;
    CHECK(readsbAcceptsUuidFrame(bOpen, bh, bl) && bh == hexOf("0f1e2d3c4b5a4968") && bl == hexOf("8796a5b4c3d2e1f0"),
          "what target B sent is not what readsb would read as its id");
    // An SBS stream is text; a binary frame in it would be garbage to whoever reads it.
    sock_t c3 = acceptWait(l3, 3000);
    CHECK(c3 != kBadSock, "the SBS target connects");
    if (c3 != kBadSock) {
        Feeds::get().adsb(frame());
        const std::string sOpen = readSome(c3, 2, 1500);
        CHECK(sOpen.size() < 2 || !((uint8_t)sOpen[0] == 0x1A && (uint8_t)sOpen[1] == 0xE4), "an SBS target was sent a binary feeder-id frame");
        sockClose(c3);
    }
    Feeds::get().adsb(frame());
    const std::string r1 = readSome(c1, 23, 2000), r2 = readSome(c2, 23, 2000);
    CHECK(r1.size() >= 23 && r1[0] == 0x1A && r1[1] == '3', "target A got a Beast frame (%zu bytes)", r1.size());
    CHECK(r1 == r2, "both targets got the same bytes (fan-out)");
    Feeds::get().ais({"!AIVDM,1,1,,A,15M67FC000G?ufbE`FepT@3n00Sa,0*5C"});
    char buf[256]; long long k = recv(u, buf, sizeof buf, 0);
    CHECK(k > 10 && std::string(buf, (size_t)k).rfind("!AIVDM", 0) == 0, "AIS datagram arrived");
    CHECK(waitFor([&] { return FeedTargets::get().status(0).sent >= 1; }, 1000), "sent counter");
    FeedTargetStatus s = FeedTargets::get().status(0);
    CHECK(s.connected && s.state.find("connected") == 0, "status connected: %s", s.state.c_str());

    // the server goes away: the target notices and reconnects with a wait
    sockClose(c1);
    CHECK(waitFor([&] { Feeds::get().adsb(frame()); return FeedTargets::get().status(0).reconnects >= 1; }, 3000), "reconnect counted after the server closed");
    sock_t c1b = acceptWait(l1, 8000);
    CHECK(c1b != kBadSock, "target reconnects");
    if (c1b != kBadSock) sockClose(c1b);

    // disabling one stops its data
    c.targets[1].enabled = false;
    Feeds::get().configure(c);
    CHECK(waitFor([&] { return FeedTargets::get().status(1).state == "off"; }, 1000), "disabled target shows off");

    Feeds::get().stop();
    if (c2 != kBadSock) sockClose(c2);
    if (l3 != kBadSock) sockClose(l3);
    printf(fails ? "feed_targets: %d failures\n" : "feed_targets: OK\n", fails);
    return fails ? 1 : 0;
}
