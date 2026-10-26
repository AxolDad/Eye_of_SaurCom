# saurcom_dmr

DMR frame layer (ETSI TS 102 361-1 / -2): the error control codes of annex B (Golay (20,8), quadratic residue (16,7,6), Hamming (7,4) and the four Hamming codes of the block
product codes, Reed-Solomon (12,9) over GF(256), CRC-CCITT, CRC-8, CRC-9, CRC-32, the embedded LC checksum), the block product turbo code BPTC (196,96), the variable length BPTC
of the embedded link control, the short LC in the CACH, the CACH burst with its TACT, the rate 3/4 trellis code (soft decode) and rate 1 data; the nine sync patterns and their matching,
the 264-bit burst assembly, slot type and EMB fields, full link control (header / terminator bursts with masked RS parity), CSBK and CRC blocks, data headers (unconfirmed, confirmed,
response, short data defined and raw / status, UDT, proprietary), data block encode / parse with the confirmed block serial number and CRC-9, the message text formats of table 9.50
(binary, BCD, 7 bit, ISO 8859-x, UTF-8 / 16 / 32) and the talker alias assembled from the embedded LC.

Modules: `bits` (bit writer / reader), `codes` (short codes, RS, CRCs; tables built at compile time), `product` (BPTC, embedded LC, short LC, CACH, trellis, rate 1), `burst`,
`lc`, `data`, `text`; `abi` holds the `repr(C)` structs, `ffi.rs` the only `unsafe`. No allocation: fixed arrays, text goes into a caller buffer through `text::Buf`.

Stays in C++: the demodulator, sync search on the matched filter output, slot tracking, calibration and call logic (`dmr_link.cpp`, `dmr_rx.cpp`, `dmr_eval.cpp`), the generator and the
radio / vocoder glue (`dmr_gen.cpp`, mbelib stays untouched), the plain bit helpers `putBits` / `getBits` / `bytesToBits` / `bitsToBytes` / `popcount32`, the sync word table `kSyncWords`, the three burst
slicers (`burstCentre`, `burstDataPayload`, `burstVoicePayload`: vector copies), `be24` / `putBe24`, and `talkerAliasPdus` (builds the test signal's alias PDUs). `dmr_fec_rs.cpp` and
`dmr_proto_rs.cpp` are the Rust-backed implementations of `dect2/dmr/dmr_fec.h` and `dmr_proto.h`; `dmr_fec.cpp` and `dmr_proto.cpp` keep only the bit helpers, the sync word table, the burst slicers and the talker alias PDU builder.
`dmr_fec.cpp` is compiled with `-ffp-contract=off` (the trellis metric adds squares) so the Rust and C++ builds agree bit for bit.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/dmr.h`, symbols prefixed `saurcom_dmr_`. Golden-vector test: `tests/dmr/test_dmr_proto_rs.cpp` (digest recorded from the C++ it replaced), fuzz target `fuzz_dmr_proto`. Status and conventions: ../README.md.
