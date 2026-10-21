// The rest of the WSJT-X family inside the HF digital receiver (JT65, JT9, Q65, FST4 / FST4W, MSK144): what hfdig_wsjt.cpp (shared tools),
// hfdig_jt65.cpp, hfdig_jt9.cpp, hfdig_q65.cpp, hfdig_fst4.cpp and hfdig_msk144.cpp share with the slot decoder (hfdig_ftx.cpp), the test
// audio and the tests. Not part of the public interface (see dect2/hfdig/hfdig_ftx.h).
//
// Sources (read, re-implemented, credited): WSJT-X (K1JT et al., GPL-3): lib/packjt.f90, lib/gen65.f90, lib/interleave63.f90, lib/igray.c,
//   lib/wrapkarn.c, lib/ftrsd/*.c (K. Karn KA9Q's Reed-Solomon codec), lib/sync65.f90, lib/jt9fano.f90, lib/gen9.f90, lib/interleave9.f90,
//   lib/jt9sync.f90, lib/qra/q65/*.c and *.f90 (Nico Palermo IV3NWV's QRA codes), lib/fst4/*.f90, lib/genmsk_128_90.f90, lib/mskrtd.f90,
//   lib/msk144*.f90, lib/ldpc_128_90_*.f90, lib/bpdecode128_90.f90.
#pragma once
#include "dect2/hfdig/hfdig_ftx.h"
#include "hfdig_ftx_int.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace wsjt {

// One slot of audio (8 kHz) to decode, with the time reference of the slot clock
struct Slot {
    const float* x = nullptr;
    size_t n = 0;
    double x0 = 0;                // the time of x[0] against the start of the slot, s
    double dtMin = -2.5, dtMax = 2.5;   // the offsets of the start of a transmission against its nominal start (s)
    int sub = 0;                  // JT65 A B C, Q65 A .. E
    double period = 60;           // T/R period of Q65 / FST4 / FST4W / MSK144, s
    double fLo = 200, fHi = 3000; // the audio band to search, Hz
    double slotUtc = 0;
};

// ---------------------------------------------------------------- tools (hfdig_wsjt.cpp)
// 8 kHz -> any rate (windowed sinc); the low-pass keeps what is below 3.7 kHz (and 46 % of the new rate)
std::vector<float> resample8k(const float* x, size_t n, double rateOut);
// Power spectrogram of y: FFT size N (a power of two), a frame every hop samples (frame f starts at sample f * hop), bins binLo .. binHi - 1
struct Spectrogram {
    int N = 0, hop = 0, binLo = 0, nBins = 0, frames = 0;
    std::vector<float> p;     // [frame][bin]
    float at(int f, int b) const { return p[(size_t)f * (size_t)nBins + (size_t)(b - binLo)]; }
    const float* row(int f) const { return &p[(size_t)f * (size_t)nBins]; }
};
Spectrogram spectrogram(const std::vector<float>& y, int N, int hop, int binLo, int binHi, bool hann = false);
// The energy of the complex correlation of y[start .. start + n) with a tone at freq (Hz, rate Hz): |sum y e^{-j 2 pi f t}|^2
double toneEnergy(const std::vector<float>& y, double rate, long start, int n, double freq);
// FFT of a symbol (N samples from start, zero outside y), the tone shifted down by shiftHz first; power of bins binLo .. binLo + nb - 1
void symbolPowers(const std::vector<float>& y, double rate, long start, int N, double shiftHz, int binLo, int nb, float* out);
// The same with a window of nwin samples zero-padded to the FFT size Nfft (a finer bin grid: bin spacing rate / Nfft)
void symbolPowersPad(const std::vector<float>& y, double rate, long start, int nwin, int Nfft, double shiftHz, int binLo, int nb, float* out);
// Gaussian-free helper: the mean of the lowest `frac` part (a robust noise level) of v
float lowMean(std::vector<float> v, double frac);
// Phase-continuous FSK: symbol k sends freqs[k] Hz for symSec seconds; the first symbol starts `start` s into out (rate Hz, may be negative or past
// the end); amplitude amp, added to out
void addFsk(const std::vector<double>& freqs, double symSec, double amp, double rate, double start, std::vector<float>& out);
// log of the modified Bessel function I0
double logI0(double x);
// Sync search of the modes with known sync tones: syncTone[i] = the tone of symbol i when it is a sync symbol, -1 for data (tone t is at bin b + t * mult
// of a candidate whose tone 0 is at bin b). The score of (bin, lag) is the mean over the sync symbols of the power against the noise level of its bin,
// minus 1 (about S/N of the bin for a signal, noise-only mean 0 and spread 1 / sqrt(n sync)); lag counts frames (frame f starts at f * hop), a symbol is
// framesPerSym frames. Returns the local maxima over the bins, best first.
struct Cand { int bin; int lag; float score; };
std::vector<Cand> syncScan(const Spectrogram& sg, const std::vector<int8_t>& syncTone, int mult, int framesPerSym, int lagLo, int lagHi, int nbSearch,
                           float minScore, size_t maxCand, int peakHalfWidth);
// Fine fit of the start (samples of y) and frequency of the sync symbols: the correlation with the sync tones, a 5 x 5 grid around (start0, f0) with
// steps of N / 16 samples and df / 4 Hz, then one 3 x 3 round with half the steps. Returns the energy of the best; start / freq are updated.
double fineFit(const std::vector<float>& y, double rate, int N, const std::vector<int8_t>& syncTone, double toneStep, long& start, double& freq, double df);
// Subtracts a signal of FSK symbols of N samples each (symbol i from start + i N, its tone freqs[i] Hz): a least-squares sinusoid fit per symbol
void subtractTones(std::vector<float>& y, double rate, long start, int N, const std::vector<double>& freqs);
bool wsjtDebug();   // the environment variable HFDIG_WSJT_DEBUG is set: the decoders print what they find
// Doppler drift (a rate of change of all frequencies in proportion: f (1 + eps t)): the track of the sync tones (offset a + D t bins, D up to +-6 bins over the
// transmission) with the most energy; f becomes the frequency in the middle of the transmission; returns eps (0 when the drift does not beat none by 15 %).
// The symbols are N samples, nsym of them from `start`, the sync tones syncTone[i] at f + tone * toneStep.
double driftSearch(const std::vector<float>& y, double rate, int N, const std::vector<int8_t>& syncTone, double toneStep, long start, double& f, double df);
// The audio with that drift taken out (the time axis warped about sample `mid`: a tone that drifted as f (1 + eps t) becomes constant)
std::vector<float> warpDrift(const std::vector<float>& y, double eps, double mid, double rate);
// callsign / grid of a decoded message for the decode record
void fillCallGrid(FtxDecode& d);

// ---------------------------------------------------------------- 72-bit messages of JT65 and JT9 (hfdig_wsjt.cpp, WSJT-X packjt.f90)
// 12 six-bit symbols. Standard messages (two calls and a locator / report / RO / RRR / 73), CQ / QRZ / DE (also CQ nnn, CQ DX, CQ aa),
// free text of up to 13 characters. Calls with a prefix or suffix (type 1 / 2 messages) are sent as free text when they fit.
bool packJt72(const std::string& text, uint8_t* sym12);
std::string unpackJt72(const uint8_t* sym12, bool* freeText = nullptr);
bool packGrid15(const std::string& grid, int& ng);   // exposed for the tests
std::string unpackGrid15(int ng);

// ---------------------------------------------------------------- LDPC codes (hfdig_wsjt_tables.cpp, hfdig_wsjt.cpp)
struct LdpcTab { int n, k, w, kb; const uint8_t* mn; const uint8_t* nm; const uint8_t* nrw; const uint8_t* gen; };
extern const LdpcTab kLdpc240_101, kLdpc240_74, kLdpc128_90;
// message bits (one per byte, k of them) -> codeword (n bits, message first, then the parity bits)
void ldpcEncode(const LdpcTab& t, const uint8_t* msg, uint8_t* cw);
// Sum-product decoding; llr > 0 means 1. Returns the number of unsatisfied parity checks (0 = a codeword) after at most iters iterations; cw = the hard decisions
int ldpcBp(const LdpcTab& t, const float* llr, int iters, uint8_t* cw);
// Ordered-statistics decoding of order `order` (0 .. 2) on the soft bits: the most reliable k independent bits are taken as the message and re-encoded;
// returns false when no codeword was found. check(cw) decides whether a candidate is accepted (CRC); the first accepted one in the order of the metric is returned
bool ldpcOsd(const LdpcTab& t, const float* llr, int order, uint8_t* cw, bool (*check)(const uint8_t* cw, void* ctx), void* ctx);

// ---------------------------------------------------------------- JT65 (hfdig_jt65.cpp)
// RS(63,12) over GF(64) (poly 0x43, first root 3), WSJT-X / Karn's order: sent[0 .. 62] = reversed codeword
void rsEncode63(const uint8_t* data12, uint8_t* sent63);                       // data12 = dgen, sent63 = the 63 symbols before interleaving
// Errors and erasures (indices into the codeword, cw[0] first = the highest power). Returns the number of symbols changed, -1 when it fails
int rsDecode63(uint8_t* cw63, const int* erasures, int nEra);
std::vector<int> jt65Tones(const std::string& msg);                            // 126 tones: 0 sync, 2 + gray(symbol) data; empty when unsendable
std::vector<FtxDecode> decodeJt65(const Slot& s);
void addJt65(const std::vector<int>& tones, int sub, double f0, double amp, double rate, double start, std::vector<float>& out);

// ---------------------------------------------------------------- JT9 (hfdig_jt9.cpp)
std::vector<int> jt9Tones(const std::string& msg);                             // 85 tones 0 .. 8 (0 = sync); empty when unsendable
std::vector<FtxDecode> decodeJt9(const Slot& s);
void addJt9(const std::vector<int>& tones, double f0, double amp, double rate, double start, std::vector<float>& out);
// K = 32, r = 1/2 convolutional code of JT9 and WSPR (polynomials 0xF2D05351, 0xE4613C47)
void convEncode32(const uint8_t* data, int nbits, uint8_t* coded);             // nbits data bits (MSB first per byte), + 31 zero tail bits
bool fano32(const float* llr, int nbits, uint8_t* data, long maxCycles);      // llr > 0 means 1, 2 * (nbits + 31) values

// ---------------------------------------------------------------- JT4 (hfdig_jt4.cpp)
std::vector<int> jt4Tones(const std::string& msg);                             // 206 tones 0 .. 3 (tone = sync bit + 2 data bit); empty when unsendable
std::vector<FtxDecode> decodeJt4(const Slot& s);                               // s.sub: 0 A .. 6 G
void addJt4(const std::vector<int>& tones, int sub, double f0, double amp, double rate, double start, std::vector<float>& out);

// ---------------------------------------------------------------- Q65 (hfdig_q65.cpp, hfdig_q65_tables.cpp)
struct QraCode {   // the Q-ary repeat-accumulate code of Q65 (qracodes.h)
    int K, N, m, M, a, NC, V, C, NMSG, MAXVDEG, MAXCDEG;
    const int *acc_input_idx, *acc_input_wlog, *gflog, *gfexp, *msgw, *vdeg, *cdeg, *v2cmidx, *c2vmidx, *gfpmat;
};
extern const QraCode kQra15_65_64;
// 13 six-bit message symbols -> the 63 channel symbols (CRC-12 added, two symbols punctured)
void q65Encode(const int* msg13, int* cw63);
// Intrinsic probabilities of the 63 symbols from the energies of their 64 tones; belief propagation; 0 = decoded (the CRC-12 checked), msg13 = the symbols
int q65Decode(const float* energies63x64, int* msg13, int maxIter, float* llhOut);
std::vector<int> q65Tones(const std::string& msg);                             // 85 tones: 0 sync, 1 + symbol data
std::vector<FtxDecode> decodeQ65(const Slot& s);
void addQ65(const std::vector<int>& tones, int sub, double period, double f0, double amp, double rate, double start, std::vector<float>& out);
double q65Baud(double period);                                                 // 12000 / nsps

// ---------------------------------------------------------------- FST4 / FST4W (hfdig_fst4.cpp)
std::vector<int> fst4Tones(const std::string& msg, bool w);                    // 160 tones 0 .. 3
std::vector<FtxDecode> decodeFst4(const Slot& s, bool w);
void addFst4(const std::vector<int>& tones, double period, double f0, double amp, double rate, double start, std::vector<float>& out);
double fst4Baud(double period);

// ---------------------------------------------------------------- MSK144 (hfdig_msk144.cpp)
bool msk144Frame(const std::string& msg, uint8_t* bits144);                    // the 144 channel bits of one frame
std::vector<FtxDecode> decodeMsk144(const Slot& s);
// the whole 15 s (or period) of repeated frames from `start` (s, in out), a burst of the given length (s) only: lenSec (0: until the end)
void addMsk144(const uint8_t* bits144, double f0, double amp, double rate, double start, double lenSec, std::vector<float>& out);

} // namespace wsjt
} // namespace dect2
