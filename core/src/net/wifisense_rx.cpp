// WiFi sensing in (wifisense_rx.h): one UDP socket, one thread, one Rust sensor per ESP32 node.
#include "dect2/net/wifisense_rx.h"
#include "dect2/common/rs_abi.h"
#include "udp_listen.h"
#include <chrono>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
constexpr size_t kMaxNodes = 64;   // RuView meshes are a handful of nodes; ids are one byte anyway
constexpr int64_t kNodeIdleMs = 60 * 1000;   // a full table gives a slot to a newcomer once a node has been silent this long

struct AbiCheck {
    AbiCheck() { rsAbiCheck("zorcom_wifisense", {zorcom_wifisense_struct_size()}, {sizeof(ZorcomWifiSense)}); }
} abiCheck;

int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

struct SensorDel { void operator()(ZorcomWifiSenseSensor* s) const { zorcom_wifisense_free(s); } };
struct Entry {
    std::unique_ptr<ZorcomWifiSenseSensor, SensorDel> sensor;
    WifiSenseListener::Node node;
};
} // namespace

struct WifiSenseListener::Impl {
    mutable std::mutex mu;
    std::map<int, Entry> nodes;
    UdpListen udp;
};

WifiSenseListener& WifiSenseListener::get() { static WifiSenseListener l; return l; }
WifiSenseListener::WifiSenseListener() : impl_(new Impl) {}
WifiSenseListener::~WifiSenseListener() { stop(); }

bool WifiSenseListener::take(const uint8_t* d, size_t n, int64_t nowMs, const std::string& from) {
    const int id = zorcom_wifisense_node(d, n);
    if (id < 0) return false;
    std::lock_guard<std::mutex> lk(impl_->mu);
    auto it = impl_->nodes.find(id);
    if (it == impl_->nodes.end()) {
        if (impl_->nodes.size() >= kMaxNodes) {   // full: drop the node silent for longest if it is idle, else ignore the newcomer
            auto old = impl_->nodes.begin();
            for (auto o = old; o != impl_->nodes.end(); ++o) if (o->second.node.heardMs < old->second.node.heardMs) old = o;
            if (nowMs - old->second.node.heardMs <= kNodeIdleMs) return true;
            impl_->nodes.erase(old);
        }
        Entry e;
        e.sensor.reset(zorcom_wifisense_new(kFramesPerSec));
        it = impl_->nodes.emplace(id, std::move(e)).first;
    }
    Entry& e = it->second;
    if (!zorcom_wifisense_feed(e.sensor.get(), d, n, &e.node.s)) return false;
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
    impl_->udp.stop();
    running_ = false;
}

bool WifiSenseListener::start(int port, std::string& err) {
    stop();
    if (!impl_->udp.open("", port, false, err)) return false;
    running_ = true;
    // the largest frame: 20 + 4 antennas x 256 subcarriers x 2
    impl_->udp.run("wifisense-rx", 4096, [this](const uint8_t* d, size_t n, const std::string& from) { take(d, n, wallMs(), from); });
    return true;
}

} // namespace dect2
