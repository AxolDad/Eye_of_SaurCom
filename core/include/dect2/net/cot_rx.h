// Cursor on Target in: listens for ATAK / WinTAK / TAK server events on a UDP port (joining a multicast group when the address is
// one, 239.2.3.1:6969 by default) and keeps each contact until its stale time. The map shows them as LayerCot markers.
#pragma once
#include "dect2/geo/maplayers.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

class CotListener {
public:
    static CotListener& get();
    ~CotListener();
    // (Re)starts listening; false with err when the port cannot be opened. group: a multicast address to join, or "" / a unicast
    // address to listen on every interface only.
    bool start(const std::string& group, int port, std::string& err);
    void stop();
    bool running() const { return running_.load(std::memory_order_relaxed); }
    uint64_t received() const { return received_.load(std::memory_order_relaxed); }
    // The contacts not yet stale at nowMs (ms since 1970); an event without a stale time stays 5 minutes after it was heard.
    std::vector<MapMarker> markers(int64_t nowMs) const;
    // What the listener does with one datagram (the tests feed it directly): false when it is not a CoT event.
    bool take(const uint8_t* d, size_t n, int64_t nowMs);

    struct Impl;
private:
    CotListener();
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> received_{0};
    std::unique_ptr<Impl> impl_;
};

} // namespace dect2
