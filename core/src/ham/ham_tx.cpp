#include "dect2/ham/ham.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include <algorithm>
#include <chrono>
#include <thread>

namespace dect2::ham {

bool HamSlotTx::busy() const { return busy_; }

bool HamSlotTx::pttSet(bool on, std::string& err) {
    std::lock_guard<std::mutex> l(mu_);
    if (pttOn_ == on) return true;
    if (!io_.ptt(on, err)) { if (!on) pttOn_ = false; return false; }
    pttOn_ = on;
    return true;
}

void HamSlotTx::abort() {
    abort_ = true;
    io_.flushAudio();
    std::string e;
    pttSet(false, e);   // released at once, from the caller's thread
}

bool HamSlotTx::send(const HamTxCheck& gateIn, int mode, const std::string& msg, double audioHz, double level, std::string& err) {
    err.clear();
    if (busy_.exchange(true)) { err = "a transmission is already running"; return false; }
    struct Done { std::atomic<bool>& b; ~Done() { b = false; } } done{busy_};
    HamTxCheck gate = gateIn;
    gate.audioHz = audioHz;
    if (!hamTxAllowed(gate, err)) return false;
    if (mode < 0 || mode > 2) { err = "not an FT8 / FT4 / FT2 mode"; return false; }
    const int rate = 48000;
    std::vector<float> a((size_t)(slotPeriod(mode) * rate) + rate, 0.f);
    const double amp = level < 0.05 ? 0.05 : level > 0.9 ? 0.9 : level;
    if (!ftxAddTransmissionEx(mode, msg, audioHz, amp, rate, 0.0, false, 0, 0, a)) { err = "the message cannot be sent: " + msg; return false; }
    size_t n = a.size();
    while (n > 0 && a[n - 1] == 0.f) n--;
    if (n == 0) { err = "empty audio"; return false; }
    abort_ = false;
    if (!io_.beginAudio(rate, err)) return false;
    if (!pttSet(true, err)) { io_.endAudio(); return false; }
    size_t pos = 0;
    while (pos < n && !abort_) {
        const int w = io_.write(a.data() + pos, (int)std::min<size_t>(n - pos, 4800));
        if (w > 0) pos += (size_t)w; else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    while (!abort_ && io_.pending() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    bool ok = true;
    if (abort_) { ok = false; err = "halted"; }
    std::string e2;
    pttSet(false, e2);
    io_.endAudio();
    if (!e2.empty() && ok) { err = e2; ok = false; }
    return ok;
}

}  // namespace dect2::ham
