# saurcom_p25

APCO Project 25 Phase 1 (TIA-102.BAAA / BAAC / BABA) frame layer: the NID (NAC + DUID, BCH (63,16,23) plus parity bit, nearest codeword decoder accepting 10 errors), the Reed-Solomon codes over GF(64)
(x^6 + x + 1, shortened, any n <= 63: header, link control, encryption sync; Berlekamp-Massey, Chien, Forney), Hamming (10,6,3) of the LDU hexbits, Golay (24,12,8) and the shortened (18,6,8), the CRC-CCITT of
TSBKs and PDU headers, the rate 1/2 trellis code with its 98-dibit interleave (soft Viterbi on C4FM levels), TSBK blocks, link control words (group, unit to unit, terminator, raw), the encryption sync and header
words (message indicator, algorithm, key id, manufacturer, talkgroup), the data unit / opcode / algorithm names, frame lengths, the IMBE voice frame interleave into mbelib's `fr[8][23]` and the channel number text.

Modules: `gf` (GF(64) tables at compile time), `rs`, `nid` (codewords through two 256-entry tables: the code is linear), `golay`, `trellis` (also the CRC), `msg`, `imbe`; `unsafe` only in `ffi.rs`. No allocation.

Stays in C++: the demodulator, frame sync search, status symbol handling and call state (`p25_rx.cpp`), the generator (`p25_gen.cpp`), the sync table `kSync` and the IMBE vocoder glue (`p25_imbe.cpp`, mbelib is not touched: the voice
bits are only de-interleaved here). `p25_proto_rs.cpp` is the Rust-backed implementation of `dect2/p25/p25_proto.h` (`p25_proto.cpp` keeps only the sync table). Reed-Solomon symbols must be in 0..63 (the C++ it replaced indexed its log table with them).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/p25.h`, symbols prefixed `saurcom_p25_`. Golden-vector test: `tests/p25/test_p25_proto_rs.cpp` (4.4 M results hashed at the default 200000 iterations per family, NID decode 100000),
fuzz target `fuzz_p25_proto`. Status and conventions: ../README.md.
