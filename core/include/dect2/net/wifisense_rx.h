// WiFi sensing in: listens for the CSI frames of RuView's ESP32 nodes (ADR-018, UDP port 5005 by default) and runs one Rust sensor
// (rust/saurcom_wifisense) per node: presence, motion, breathing and heart rate for each room a node watches.
#pragma once
#include "dect2/rs/wifisense.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class WifiSenseListener {
public:
    static constexpr int kDefaultPort = 5005;
    static constexpr double kFramesPerSec = 50.0;   // RuView firmware default

    struct Node {
        SaurcomWifiSense s{};
        int64_t heardMs = 0;   // ms since 1970 of the last frame
        std::string from;      // sender address
    };

    static WifiSenseListener& get();
    ~WifiSenseListener();
    // (Re)starts listening on every interface; false with err when the port cannot be opened.
    bool start(int port, std::string& err);
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }
    uint64_t received() const { return received_.load(std::memory_order_relaxed); }
    // The nodes heard, by node id.
    std::vector<Node> nodes() const;
    // Forgets every node (their sensors start again).
    void clear();
    // What the listener does with one datagram (the tests feed it directly): false when it is not a CSI frame.
    bool take(const uint8_t* d, size_t n, int64_t nowMs, const std::string& from = {});

    struct Impl;
private:
    WifiSenseListener();
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> received_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace dect2
