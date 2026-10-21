// COSPAS-SARSAT second-generation beacon test signal (see sarsat_gen.h). Nothing is transmitted.
#include "dect2/sarsat/sarsat_gen.h"
#include "dect2/common/mathconst.h"
#include <cmath>

namespace dect2::sarsat {

using dect2::math::kPi;

std::vector<SgbSpec> sgbTestBeacons() {
    std::vector<SgbSpec> v;
    {   // EPIRB off the Lithuanian coast
        SgbSpec s; s.tac = 4711; s.serial = 1234; s.mid = 277; s.beaconType = 1; s.homing = true; s.lat = 55.7041; s.lon = 20.9873; s.rotId = 0; s.rotData = 0x123456789ull;
        v.push_back(s);
    }
    {   // PLB in the southern hemisphere, western longitude
        SgbSpec s; s.tac = 65535; s.serial = 16383; s.mid = 512; s.beaconType = 2; s.lat = -41.2865; s.lon = -174.7762; s.rotId = 1;
        v.push_back(s);
    }
    {   // ELT, no position yet, with a vessel / aircraft identification
        SgbSpec s; s.tac = 230; s.serial = 573; s.mid = 201; s.beaconType = 0; s.hasPos = false; s.vesselType = 3; s.vesselId = 0x3C6444123ull; s.homing = true; s.rls = true; s.rotId = 2;
        v.push_back(s);
    }
    {   // a test-protocol PLB
        SgbSpec s; s.tac = 100; s.serial = 7; s.mid = 366; s.beaconType = 2; s.test = true; s.lat = 37.7749; s.lon = -122.4194;
        v.push_back(s);
    }
    return v;
}

std::vector<std::complex<float>> sgbBurstSamples(const std::vector<uint8_t>& bits, double rate, double carrierHz, bool selfTest, double chipPpm) {
    const std::vector<uint8_t> pi = sgbPrn(false, selfTest), pq = sgbPrn(true, selfTest);
    const double chipRate = kSgbChipRate * (1.0 + chipPpm * 1e-6);
    const size_t n = (size_t)std::llround(rate * (kSgbChips / kSgbChipRate)) + (size_t)(rate * 0.002);
    std::vector<std::complex<float>> out(n);
    auto chipVal = [&](const std::vector<uint8_t>& prn, double chipPos, int parity) -> double {
        if (chipPos < 0) return 0.0;
        const long c = (long)chipPos;
        if (c >= kSgbChips) return 0.0;
        const long sym = c / 256;   // channel bit number 0..149
        int bit = 0;
        if (sym >= 25) { const size_t idx = (size_t)(2 * (sym - 25) + parity); bit = idx < bits.size() ? (bits[idx] & 1) : 0; }
        return 1.0 - 2.0 * (double)(prn[(size_t)c] ^ bit);
    };
    double ph = 0;
    const double w = 2 * kPi * carrierHz / rate;
    for (size_t i = 0; i < n; i++) {
        const double cp = (double)i / rate * chipRate;
        const double a = chipVal(pi, cp, 0), b = chipVal(pq, cp - 0.5, 1);
        const double amp = 0.70710678;
        out[i] = std::complex<float>((float)(amp * (a * std::cos(ph) - b * std::sin(ph))), (float)(amp * (a * std::sin(ph) + b * std::cos(ph))));
        ph += w;
        if (ph > kPi) ph -= 2 * kPi; else if (ph < -kPi) ph += 2 * kPi;
    }
    return out;
}

}  // namespace dect2::sarsat
