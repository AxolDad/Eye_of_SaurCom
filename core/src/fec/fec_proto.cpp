// The K = 7 Viterbi decoder of DVB-T / ISDB-T (see fec_proto.h): NEON / SSE2 / AVX2 / portable. The Reed-Solomon, BCH and LDPC parts of the shared forward error
// correction are Rust (fec_proto_rs.cpp over rust/saurcom_fec). The Rust NEON Viterbi kernel (SAURCOM_RUST_VITERBI, aarch64) is ~4% slower than this one, so it stays opt-in
// (cmake -DDECT2_RUST_VITERBI=ON) and this C++ decoder is the one in use by default.
#include "dect2/fec/fec_proto.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#if !defined(SAURCOM_RUST_VITERBI) || !defined(__aarch64__)
#if defined(__SSE2__) && !defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <emmintrin.h>
#if defined(__GNUC__) && defined(__x86_64__)
#include <immintrin.h>
#define DECT2_VIT_AVX2 1
#endif
#endif
#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#endif
namespace dect2::fecp {
namespace {
struct Trellis {
    // for each new state ns and predecessor bit b: expected code bits (x,y) as +1 / -1 signs
    int8_t sx[64][2], sy[64][2];
    Trellis() {
        for (int ns = 0; ns < 64; ns++) {
            const int u = ns >> 5;
            for (int b = 0; b < 2; b++) {
                const int s = ((ns & 31) << 1) | b;           // predecessor state
                const unsigned reg = ((unsigned)u << 6) | (unsigned)s;
                sx[ns][b] = __builtin_parity(reg & 0x79) ? -1 : 1;
                sy[ns][b] = __builtin_parity(reg & 0x5B) ? -1 : 1;
            }
        }
    }
};
const Trellis& trellis() { static Trellis t; return t; }

#if defined(__ARM_NEON) && !defined(DECT2_NO_SIMD)
#include <arm_neon.h>
#include <array>
#include <cstdint>
#include <vector>
// NEON path. States 2j and 2j+1 feed new states j and j+32; both branch outputs flip when either the new bit or the oldest
// state bit flips (both generator polynomials have those taps), so one metric M_j = +/-sx +/- sy per butterfly is enough:
//   new[j]    = max(old[2j] + M, old[2j+1] - M)      new[j+32] = max(old[2j] - M, old[2j+1] + M)
struct NeonTables {
    int16_t sigX[32], sigY[32];
    NeonTables() {
        for (int j = 0; j < 32; j++) {
            const unsigned reg = (unsigned)(2 * j);
            sigX[j] = __builtin_parity(reg & 0x79) ? -1 : 1;
            sigY[j] = __builtin_parity(reg & 0x5B) ? -1 : 1;
        }
    }
};
const NeonTables& neonTables() { static NeonTables t; return t; }


void viterbiRangeCpp(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const NeonTables& T = neonTables();
    std::vector<uint64_t> dec(steps);
    long off = 0; // total subtracted by the renormalisation: the absolute path metric is off + pm
    alignas(16) int16_t pm[64] = {};
    int16x8_t sigX[4], sigY[4];
    for (int q = 0; q < 4; q++) { sigX[q] = vld1q_s16(T.sigX + 8 * q); sigY[q] = vld1q_s16(T.sigY + 8 * q); }
    // The 64 path metrics live in eight registers for the whole run (no store and reload per step: that round trip made the
    // chain of steps latency-bound). N[q] holds states 8q..8q+7.
    int16x8_t N[8];
    for (int q = 0; q < 8; q++) N[q] = vdupq_n_s16(0);
    static const uint8_t w8[16] = {1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128};
    const uint8x16_t wt = vld1q_u8(w8);
    for (size_t t = 0; t < steps; t++) {
        const int16_t a = soft[2 * t], b = soft[2 * t + 1];
        int16x8_t nn[8];
        uint16x8_t lo[4], hi[4];
        for (int q = 0; q < 4; q++) {
            const int16x8x2_t o = vuzpq_s16(N[2 * q], N[2 * q + 1]);   // even states, odd states
            const int16x8_t M = vmlaq_n_s16(vmulq_n_s16(sigX[q], a), sigY[q], b);
            const int16x8_t A = o.val[0], B = o.val[1];
            const int16x8_t p0 = vqaddq_s16(A, M), p1 = vqsubq_s16(B, M);
            const int16x8_t r0 = vqsubq_s16(A, M), r1 = vqaddq_s16(B, M);
            nn[q] = vmaxq_s16(p0, p1);
            nn[4 + q] = vmaxq_s16(r0, r1);
            lo[q] = vcgtq_s16(p1, p0);
            hi[q] = vcgtq_s16(r1, r0);
        }
        // the 64 decision bits (lane i of the 64 comparison results -> bit i): narrow to bytes, weight by 1,2,4..128 and add
        // neighbours three times, which leaves each group of eight lanes as one byte
        const uint8x16_t c0 = vandq_u8(vcombine_u8(vmovn_u16(lo[0]), vmovn_u16(lo[1])), wt);
        const uint8x16_t c1 = vandq_u8(vcombine_u8(vmovn_u16(lo[2]), vmovn_u16(lo[3])), wt);
        const uint8x16_t c2 = vandq_u8(vcombine_u8(vmovn_u16(hi[0]), vmovn_u16(hi[1])), wt);
        const uint8x16_t c3 = vandq_u8(vcombine_u8(vmovn_u16(hi[2]), vmovn_u16(hi[3])), wt);
        uint8x16_t r = vpaddq_u8(vpaddq_u8(c0, c1), vpaddq_u8(c2, c3));
        r = vpaddq_u8(r, r);
        dec[t] = vgetq_lane_u64(vreinterpretq_u64_u8(r), 0);
        // renormalise every 4 steps so int16 never saturates
        if ((t & 3) == 3) {
            int16x8_t mx = nn[0];
            for (int q = 1; q < 8; q++) mx = vmaxq_s16(mx, nn[q]);
            const int16_t top = vmaxvq_s16(mx);
            off += top;
            const int16x8_t m = vdupq_n_s16(top);
            for (int q = 0; q < 8; q++) N[q] = vsubq_s16(nn[q], m);
        } else for (int q = 0; q < 8; q++) N[q] = nn[q];
    }
    for (int q = 0; q < 8; q++) vst1q_s16(pm + 8 * q, N[q]);
    int state = 0;
    int bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#elif defined(__SSE2__) && !defined(DECT2_NO_SIMD)
// SSE2 path (every x86-64 CPU): the same butterflies and int16 metrics as the NEON path above, eight states per register.
struct SseTables {
    alignas(16) int16_t sigX[32], sigY[32];
    SseTables() {
        for (int j = 0; j < 32; j++) {
            const unsigned reg = (unsigned)(2 * j);
            sigX[j] = __builtin_parity(reg & 0x79) ? -1 : 1;
            sigY[j] = __builtin_parity(reg & 0x5B) ? -1 : 1;
        }
    }
};
const SseTables& sseTables() { static SseTables t; return t; }

// states 16q..16q+15: the even ones (old[2j]) to A, the odd ones (old[2j+1]) to B, like NEON's vld2q_s16
inline void deinterleave(const __m128i x0, const __m128i x1, __m128i& A, __m128i& B) {
    A = _mm_packs_epi32(_mm_srai_epi32(_mm_slli_epi32(x0, 16), 16), _mm_srai_epi32(_mm_slli_epi32(x1, 16), 16));
    B = _mm_packs_epi32(_mm_srai_epi32(x0, 16), _mm_srai_epi32(x1, 16));
}
// one bit per lane of a comparison result, lane i to bit i
inline uint8_t maskBits(__m128i m) { return (uint8_t)_mm_movemask_epi8(_mm_packs_epi16(m, _mm_setzero_si128())); }
inline int16_t maxLane(__m128i v) {
    v = _mm_max_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(1, 0, 3, 2)));
    v = _mm_max_epi16(v, _mm_shuffle_epi32(v, _MM_SHUFFLE(2, 3, 0, 1)));
    v = _mm_max_epi16(v, _mm_shufflelo_epi16(v, _MM_SHUFFLE(2, 3, 0, 1)));
    return (int16_t)_mm_cvtsi128_si32(v);
}

void viterbiRangeSse(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const SseTables& T = sseTables();
    std::vector<uint64_t> dec(steps);
    long off = 0; // total subtracted by the renormalisation: the absolute path metric is off + pm
    alignas(16) int16_t pm[64] = {};
    __m128i sigX[4], sigY[4];
    for (int q = 0; q < 4; q++) { sigX[q] = _mm_load_si128((const __m128i*)(T.sigX + 8 * q)); sigY[q] = _mm_load_si128((const __m128i*)(T.sigY + 8 * q)); }
    // the 64 path metrics stay in eight registers (N[q] = states 8q..8q+7) instead of a store and reload per step
    __m128i N[8];
    for (int q = 0; q < 8; q++) N[q] = _mm_setzero_si128();
    for (size_t t = 0; t < steps; t++) {
        const __m128i a = _mm_set1_epi16(soft[2 * t]), b = _mm_set1_epi16(soft[2 * t + 1]);
        __m128i nn[8];
        uint64_t d = 0;
        for (int q = 0; q < 4; q++) {
            __m128i A, B;
            deinterleave(N[2 * q], N[2 * q + 1], A, B);
            const __m128i M = _mm_add_epi16(_mm_mullo_epi16(sigX[q], a), _mm_mullo_epi16(sigY[q], b));
            const __m128i p0 = _mm_adds_epi16(A, M), p1 = _mm_subs_epi16(B, M);
            const __m128i r0 = _mm_subs_epi16(A, M), r1 = _mm_adds_epi16(B, M);
            nn[q] = _mm_max_epi16(p0, p1);
            nn[4 + q] = _mm_max_epi16(r0, r1);
            d |= (uint64_t)maskBits(_mm_cmpgt_epi16(p1, p0)) << (8 * q);
            d |= (uint64_t)maskBits(_mm_cmpgt_epi16(r1, r0)) << (32 + 8 * q);
        }
        dec[t] = d;
        // renormalise every 4 steps so int16 never saturates
        if ((t & 3) == 3) {
            __m128i mx = nn[0];
            for (int q = 1; q < 8; q++) mx = _mm_max_epi16(mx, nn[q]);
            const int16_t top = maxLane(mx);
            off += top;
            const __m128i m = _mm_set1_epi16(top);
            for (int q = 0; q < 8; q++) N[q] = _mm_sub_epi16(nn[q], m);
        } else for (int q = 0; q < 8; q++) N[q] = nn[q];
    }
    for (int q = 0; q < 8; q++) _mm_store_si128((__m128i*)(pm + 8 * q), N[q]);
    int state = 0;
    int bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#ifdef DECT2_VIT_AVX2
// The same trellis with 256-bit registers (16 states each): identical decisions and metrics, half the instructions.
__attribute__((target("avx2")))
inline void deinterleave256(const __m256i x0, const __m256i x1, __m256i& A, __m256i& B) {
    // packs works within the 128-bit halves, so the quarters come out as 0 2 1 3 and are put back in order
    const __m256i a = _mm256_packs_epi32(_mm256_srai_epi32(_mm256_slli_epi32(x0, 16), 16), _mm256_srai_epi32(_mm256_slli_epi32(x1, 16), 16));
    const __m256i b = _mm256_packs_epi32(_mm256_srai_epi32(x0, 16), _mm256_srai_epi32(x1, 16));
    A = _mm256_permute4x64_epi64(a, 0xD8);
    B = _mm256_permute4x64_epi64(b, 0xD8);
}
// the comparison results of two registers (32 lanes) as 32 bits, lane i to bit i
__attribute__((target("avx2")))
inline uint32_t maskBits256(__m256i m0, __m256i m1) {
    return (uint32_t)_mm256_movemask_epi8(_mm256_permute4x64_epi64(_mm256_packs_epi16(m0, m1), 0xD8));
}

__attribute__((target("avx2")))
void viterbiRangeAvx2(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const SseTables& T = sseTables();
    std::vector<uint64_t> dec(steps);
    long off = 0;
    alignas(32) int16_t pm[64] = {};
    const __m256i sigX0 = _mm256_loadu_si256((const __m256i*)T.sigX), sigX1 = _mm256_loadu_si256((const __m256i*)(T.sigX + 16));
    const __m256i sigY0 = _mm256_loadu_si256((const __m256i*)T.sigY), sigY1 = _mm256_loadu_si256((const __m256i*)(T.sigY + 16));
    __m256i N0 = _mm256_setzero_si256(), N1 = N0, N2 = N0, N3 = N0;   // states 0-15, 16-31, 32-47, 48-63
    for (size_t t = 0; t < steps; t++) {
        const __m256i a = _mm256_set1_epi16(soft[2 * t]), b = _mm256_set1_epi16(soft[2 * t + 1]);
        __m256i A0, B0, A1, B1;
        deinterleave256(N0, N1, A0, B0);   // old states 0-31: new states 0-15 and 32-47
        deinterleave256(N2, N3, A1, B1);   // old states 32-63: new states 16-31 and 48-63
        const __m256i M0 = _mm256_add_epi16(_mm256_mullo_epi16(sigX0, a), _mm256_mullo_epi16(sigY0, b));
        const __m256i M1 = _mm256_add_epi16(_mm256_mullo_epi16(sigX1, a), _mm256_mullo_epi16(sigY1, b));
        const __m256i p00 = _mm256_adds_epi16(A0, M0), p10 = _mm256_subs_epi16(B0, M0);
        const __m256i r00 = _mm256_subs_epi16(A0, M0), r10 = _mm256_adds_epi16(B0, M0);
        const __m256i p01 = _mm256_adds_epi16(A1, M1), p11 = _mm256_subs_epi16(B1, M1);
        const __m256i r01 = _mm256_subs_epi16(A1, M1), r11 = _mm256_adds_epi16(B1, M1);
        __m256i n0 = _mm256_max_epi16(p00, p10), n1 = _mm256_max_epi16(p01, p11);
        __m256i n2 = _mm256_max_epi16(r00, r10), n3 = _mm256_max_epi16(r01, r11);
        dec[t] = (uint64_t)maskBits256(_mm256_cmpgt_epi16(p10, p00), _mm256_cmpgt_epi16(p11, p01))
               | (uint64_t)maskBits256(_mm256_cmpgt_epi16(r10, r00), _mm256_cmpgt_epi16(r11, r01)) << 32;
        if ((t & 3) == 3) {
            __m256i mx = _mm256_max_epi16(_mm256_max_epi16(n0, n1), _mm256_max_epi16(n2, n3));
            const int16_t top = maxLane(_mm_max_epi16(_mm256_castsi256_si128(mx), _mm256_extracti128_si256(mx, 1)));
            off += top;
            const __m256i m = _mm256_set1_epi16(top);
            n0 = _mm256_sub_epi16(n0, m); n1 = _mm256_sub_epi16(n1, m); n2 = _mm256_sub_epi16(n2, m); n3 = _mm256_sub_epi16(n3, m);
        }
        N0 = n0; N1 = n1; N2 = n2; N3 = n3;
    }
    _mm256_store_si256((__m256i*)pm, N0); _mm256_store_si256((__m256i*)(pm + 16), N1);
    _mm256_store_si256((__m256i*)(pm + 32), N2); _mm256_store_si256((__m256i*)(pm + 48), N3);
    int state = 0;
    int bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#endif

void viterbiRangeCpp(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
#ifdef DECT2_VIT_AVX2
    static const bool avx2 = __builtin_cpu_supports("avx2");
    if (avx2) return viterbiRangeAvx2(soft, steps, bits, metricOut);
#endif
    viterbiRangeSse(soft, steps, bits, metricOut);
}
#else
void viterbiRangeCpp(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) {
    const Trellis& T = trellis();
    std::vector<uint64_t> dec(steps);
    long off = 0;
    int32_t pm[64] = {}, nm[64];
    for (size_t t = 0; t < steps; t++) {
        const int sx = soft[2 * t], sy = soft[2 * t + 1];
        uint64_t d = 0;
        for (int ns = 0; ns < 64; ns++) {
            const int p0 = ((ns & 31) << 1), p1 = p0 | 1;
            const int m0 = pm[p0] + T.sx[ns][0] * sx + T.sy[ns][0] * sy;
            const int m1 = pm[p1] + T.sx[ns][1] * sx + T.sy[ns][1] * sy;
            if (m1 > m0) { nm[ns] = m1; d |= 1ull << ns; } else nm[ns] = m0;
        }
        int best = nm[0];
        for (int s = 1; s < 64; s++) best = std::max(best, nm[s]);
        off += best;
        for (int s = 0; s < 64; s++) pm[s] = nm[s] - best;
        dec[t] = d;
    }
    int state = 0, bm = pm[0];
    for (int s = 1; s < 64; s++) if (pm[s] > bm) { bm = pm[s]; state = s; }
    bits.assign(steps, 0);
    for (size_t t = steps; t-- > 0;) {
        bits[t] = (uint8_t)(state >> 5);
        const int b = (int)((dec[t] >> state) & 1);
        state = ((state & 31) << 1) | b;
    }
    if (metricOut) *metricOut = off + bm;
}
#endif
} // namespace


void viterbiRange(const int8_t* soft, size_t steps, std::vector<uint8_t>& bits, long* metricOut) { viterbiRangeCpp(soft, steps, bits, metricOut); }
} // namespace dect2::fecp
#endif // C++ Viterbi
