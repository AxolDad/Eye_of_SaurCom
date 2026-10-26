# saurcom_acars

ACARS (ARINC 618) block layer: the block check sequence (CRC-16 KERMIT) and single-bit syndromes, odd parity, parity / check-sequence repair
(`repair`), block field parsing with address, label, block id, downlink message number and flight id, text and the H1 sublabel / MFI (`block`), label names and
the short reading of the Q-label OOOI texts and the SA media advisory (`label`).

No allocation, no panics, `unsafe` only in `ffi.rs`. `acarsBuildFrame` (the transmitter of the test signal) stays in `core/src/acars/acars_proto.cpp`;
`acars_proto_rs.cpp` is the Rust-backed implementation of the rest of `dect2/acars/acars_proto.h`.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/acars.h`, symbols prefixed `saurcom_acars_`. Golden-vector test: `tests/acars/test_acars_proto_rs.cpp` (about 2.8 M
comparisons), fuzz target `fuzz_acars_proto`. Status and conventions: ../README.md.
