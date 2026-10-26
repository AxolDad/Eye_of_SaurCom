# saurcom_fec

The shared forward error correction that is pure and hot, moved out of `dvbt.cpp`, `atsc.cpp`, `dab_fec.cpp`, `t2fec.cpp` and `dsp/ldpc.cpp` into one layer
(`core/include/dect2/fec/fec_proto.h`, namespace `dect2::fecp`):

* **Reed-Solomon over GF(256)** (field polynomial 0x11D, roots alpha^0 .. alpha^(R-1), shortened from n = 255): one generic encoder / decoder instantiated for DVB-T and ISDB-T
  RS(204,188) (EN 300 744 section 4.4), ATSC RS(207,187) (A/53 part 2 section 6.4.1) and DAB RS(120,110) (EN 300 401 section 5.3.3). Tables are `const fn` data, no start-up cost.
  The decoder is syndromes from the division remainder, Berlekamp-Massey, Chien search, Forney, then a re-check; it writes corrections as it finds them (a failed decode can leave a
  partly corrected word, exactly like the C++), only the DAB one keeps the word untouched on failure.
* **DVB-T2 BCH** (EN 302 755 section 6.1.1): normal frames GF(2^16), short frames GF(2^14), t = 1..12. Encoder = remainder of message * x^deg by a byte table, decoder = remainder, syndromes,
  Berlekamp-Massey, a block-wise Chien search (no divisions). The 2^16 field tables live in a caller owned `BchCtx` (the C++ `Bch` object allocates `saurcom_fec_bch_ctx_size()` bytes and
  calls `saurcom_fec_bch_init` once per code).
* **K = 7 Viterbi** (171, 133 octal) of DVB-T / ISDB-T: `i16` saturating metrics, renormalised every 4 steps, decisions as one `u64` per step, trace-back. `viterbi_neon.rs` is the
  aarch64 kernel (the only `unsafe` outside `ffi.rs`: `core::arch` intrinsics are `unsafe fn`s), `viterbi::range_scalar` the portable twin the tests compare it with.
* **LDPC parity-check tables** of the DVB-T2 / DVB-S2 IRA codes (360 bit groups): the CSR check -> variable lists, the encoder edge list and the layer lists the decoders use
  (`ldpc.rs`). **The LDPC decoders stay in C++** (see below).

No allocation anywhere: callers pass (pointer, length) buffers; failures return `false` / `-2` / `usize::MAX`, never crash. `unsafe` only in `ffi.rs` and `viterbi_neon.rs`.

## Where Rust is the default and where the C++ stays

Measured with `bench_fec_proto` (CPU time of the thread, best of 7, Apple M-series; Rust column first, C++ second, ns per call):

| What | Rust | C++ | Default |
|---|---|---|---|
| RS(204,188) encode / decode 0 / 3 / 8 errors | 991 / 997 / 3388 / 4126 | 996 / 999 / 2881 / 4089 | Rust (same speed) |
| RS(207,187) encode / decode 0 / 5 / 10 errors | 1162 / 1222 / 4309 / 6094 | 1155 / 1148 / 4075 / 5755 | Rust (same speed) |
| RS(120,110) encode / decode 0 / 3 / 5 errors | 657 / 686 / 1794 / 2154 | 627 / 2794 / 6932 / 7001 | Rust (up to 4x faster decode) |
| LDPC tables 2/3 normal, 5/6 normal, 1/2 short | 0.51, 0.53, 0.10 ms | 3.3, 2.6, 0.9 ms | Rust (5-9x faster) |
| BCH normal t=12 encode / decode 0 errors | 31 us / 31 us | 36 us / 32 us | Rust |
| BCH normal t=12 decode 6 / 12 errors | 0.50 / 0.89 ms | 0.35 / 0.58 ms | see the note below |
| Viterbi 1000 / 16768 steps (aarch64 NEON) | 7.46 / 125 us | 7.19 / 120 us | **C++** (Rust is 4% slower; Rust kernel opt-in with `-DDECT2_RUST_VITERBI=ON`) |

The Rust BCH decoder with many errors is slower than the C++ one (the Chien search dominates; the C++ compiler vectorises its table walk better than the blocked Rust one) but a
frame with 6 or more errors is the rare case; the common error-free and encode paths are faster.

LDPC decoders (`LdpcCode::decode*`, NEON / AVX2 / Metal / D3D11): not converted. They are written for the vector units and the GPU; a scalar Rust version would be slower, and a
`core::arch` version would only duplicate the C++ one. Only the table builder moved.

## Stays in C++

The LDPC decoders and GPU paths, the T2 / S2 / ATSC / ISDB-T receivers and the interleavers around these codes, the DAB / ATSC trellis-code decoders (different trellis structures,
SIMD bodies of `dect2/dsp/simd.h`), the SSE2 / AVX2 Viterbi (x86: the portable Rust version would be slower than the intrinsics, so `viterbiRange` is C++ there), and the per-mode
Viterbi / Reed-Solomon copies of other modes (DMR, P25, NXDN, D-STAR, VDL2 are in their own crates already; the remaining small Viterbi decoders of WiFi, HaLow, M17, TETRA, GNSS,
cellular, DRM live in their mode files with their own puncturing).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/fec.h`, symbols prefixed `saurcom_fec_`. C++ side: `core/src/fec/fec_proto.cpp` (the Viterbi decoder only, `#if !defined(SAURCOM_RUST_VITERBI) || !defined(__aarch64__)`), `core/src/fec/fec_proto_rs.cpp` (everything else). Golden-vector test `tests/fec/test_fec_proto_rs.cpp`
(100000 inputs per family, plus the real DVB-T2 tables, the Rust Viterbi kernel against the C++ one on aarch64 and the C ABI misuse cases), fuzz target `fuzz_fec_proto`.
Status and conventions: ../README.md.
