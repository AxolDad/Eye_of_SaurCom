// Recent decoded events kept as JSON text (one line each) so a station can stream them to a collecting station.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "dect2/automation/event.h"

namespace dect2 {

// {"type":..,"time":..,"mode":..,"freq":..,"f":{...}} on one line.
std::string eventToJson(const automation::Event& e);
// Parses one line. False (err set) for bad JSON, a missing type, an "f" that is not an object or holds nested values.
// Strings are cut at automation::kMaxFieldText.
bool eventFromJson(const std::string& line, automation::Event& out, std::string& err);

// Fixed-size ring of serialised events. Every pushed event gets the next sequence number (head() counts them).
class EventRing {
public:
    explicit EventRing(size_t cap = 4096);
    // Events that carry f["site"] came from another station and are not stored again.
    void push(const automation::Event& e);
    // Appends up to max lines from sequence `cursor` on and advances it. `missed` is the count of events that fell out of
    // the ring before they could be read (cursor jumps over them). A cursor past head() restarts at head(). Returns lines added.
    size_t since(uint64_t& cursor, std::vector<std::string>& lines, size_t max, uint64_t& missed) const;
    uint64_t head() const;

private:
    mutable std::mutex mu_;
    std::vector<std::string> slots_;
    uint64_t head_ = 0;
};

EventRing& globalEventRing();

}  // namespace dect2
