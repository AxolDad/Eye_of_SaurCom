# saurcom_pager

POCSAG (ITU-R M.584-2) and FLEX (public descriptions of the protocol; no free specification): the BCH(31,21) code word with parity (encode, check, repair of up to two bad
bits), POCSAG word classification (idle / address / data), the 7-bit alphanumeric and 4-bit numeric readers, the "does this look like real text" heuristic of the auto read
mode, the FLEX speed table (sync code to baud / levels / speed), frame information word (FIW) checksum, and the decoding of one FLEX phase (88 words, 8-way interleave,
block information word, short and long addresses, vector words, alphanumeric / numeric / short numeric / tone / secure / binary pages).

Modules: `bch`, `pocsag`, `text`, `flex`. No allocation (a page is a fixed 320-byte text), no panics, `unsafe` only in `ffi.rs`.

Stays in C++ (`core/src/pager/pager_proto.cpp`, built in every build): the generators `pocsagBits` and `flexPhaseBits` (transmitter of the test signal, they call the Rust
`encodeWord`), the `PocsagParser` message assembler (its bit list grows without a bound, so it stays a C++ vector; the per-word decision is `pocsagClassify` in Rust), and the
demodulator / receiver. `core/src/pager/pager_proto_rs.cpp` is the Rust-backed implementation of the rest of `dect2/pager/pager_proto.h`.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/pager.h`, symbols prefixed `saurcom_pager_`. Golden-vector test: `tests/pager/test_pager_proto_rs.cpp` (about 3.8 M
comparisons), fuzz target `fuzz_pager_proto`. Status and conventions: ../README.md.
