// WiFi sensing in (wifisense_rx.h): one UDP socket, one thread, one Rust sensor per ESP32 node.
#include "dect2/net/wifisense_rx.h"
#include "dect2/common/thread_name.h"
#include "netcompat.h"
#include "tcp_client.h"
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

namespace dect2 {

namespace {
constexpr size_t kMaxNodes = 64;   // RuView meshes are a handful of nodes; ids are one byte anyway

int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct SensorDel { void operator()(OnairWifiSenseSensor* s) const { onair_wifisense_free(s); } };
struct Entry {
    std::unique_ptr<OnairWifiSenseSensor, SensorDel> sensor;
    WifiSenseListener::Node node;
};
} // namespace

struct WifiSenseListener::Impl {
    mutable std::mutex mu;
    std::map<int, Entry> nodes;
    sock_t s = kBadSock;
    std::thread th;
    std::atomic<bool> quit{false};
};

WifiSenseListener& WifiSenseListener::get() { static WifiSenseListener l; return l; }
WifiSenseListener::WifiSenseListener() : impl_(new Impl) {}
WifiSenseListener::~WifiSenseListener() { stop(); }

bool WifiSenseListener::take(const uint8_t* d, size_t n, int64_t nowMs, const std::string& from) {
    const int id = onair_wifisense_node(d, n);
    if (id < 0) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->nodes.find(id);
    if (it == impl_->nodes.end()) {
        if (impl_->nodes.size() >= kMaxNodes) return true;
        Entry e;
        e.sensor.reset(onair_wifisense_new(kFramesPerSec));
        it = impl_->nodes.emplace(id, std::move(e)).first;
    }
    Entry& e = it->second;
    if (!onair_wifisense_feed(e.sensor.get(), d, n, &e.node.s)) return false;
    e.node.heardMs = nowMs;
    if (!from.empty()) e.node.from = from;
    received_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

std::vector<WifiSenseListener::Node> WifiSenseListener::nodes() const {
    std::vector<Node> v;
    std::lock_guard<std::mutex> lk(impl_->mu);
    for (const auto& [id, e] : impl_->nodes) v.push_back(e.node);
    return v;
}

void WifiSenseListener::clear() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    impl_->nodes.clear();
}

void WifiSenseListener::stop() {
    impl_->quit = true;
    if (impl_->th.joinable()) impl_->th.join();
    if (impl_->s != kBadSock) { sockClose(impl_->s); impl_->s = kBadSock; }
    running_ = false;
}

bool WifiSenseListener::start(int port, std::string& err) {
    stop();
    netInit();
    if (onair_wifisense_struct_size() != sizeof(OnairWifiSense)) { err = "WiFi sensing library mismatch"; return false; }
    if (port <= 0 || port > 65535) { err = "port out of range"; return false; }
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kBadSock) { err = "no UDP socket"; return false; }
    sockaddr_in a{};
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, (const sockaddr*)&a, sizeof a) != 0) { sockClose(s); err = "cannot listen on UDP port " + std::to_string(port); return false; }
    sockRecvTimeout(s, 300);   // so stop() is noticed
    impl_->s = s;
    impl_->quit = false;
    running_ = true;
    impl_->th = std::thread([this] {
        setThreadName("wifisense-rx");
        std::vector<uint8_t> buf(4096);   // the largest frame: 20 + 4 antennas x 256 subcarriers x 2
        while (!impl_->quit) {
            sockaddr_in src{};
            socklen_t sl = sizeof src;
            const long long n = recvfrom(impl_->s, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&src, &sl);
            if (n <= 0) continue;
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &src.sin_addr, ip, sizeof ip);
            take(buf.data(), (size_t)n, wallMs(), ip);
        }
    });
    return true;
}

} // namespace dect2
