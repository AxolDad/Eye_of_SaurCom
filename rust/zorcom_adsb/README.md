# zorcom_adsb

ADS-B / Mode S (1090 MHz) message layer: CRC-24 and single-bit correction (`crc`), altitude / identity / character / category codes (`altitude`), compact position
reporting with global, local and surface decoding and the encoders (`cpr`), Comm-B BDS register inference and the advisory text (`commb`), great circle, magnetic
declination, wind triangle and ISA models (`geo`), field extraction of DF0/4/5/11/16/17/18/19/20/21 including every extended squitter type code (`msg`).

No allocation, no panics, `unsafe` only in `ffi.rs`, which also declares the few C math functions that `no_std` Rust lacks (`math`): the C++ reference uses the same ones, and
`adsb_msg.cpp` is compiled with `-ffp-contract=off -fno-builtin-sin -fno-builtin-cos` so both give the same bits (see ../README.md). The CPR zone table is stored as computed by the
formula (a unit test checks it). `getBits` / `setBits` / `toHex` stay in `core/src/adsb/adsb_msg.cpp`; `adsb_msg_rs.cpp` is the Rust-backed implementation of the rest of
`dect2/adsb/adsb_msg.h`. UAT, the encoder / generator, tracker and register stay in C++.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/adsb.h` (the structs `ZorcomAdsbMsg` / `ZorcomAdsbCommB` mirror the Rust `repr(C)` structs, sizes are checked at start-up).
Golden-vector test: `tests/adsb/test_adsb_proto_rs.cpp` (about 5.6 M results hashed), fuzz target `fuzz_adsb_proto`. Status and conventions: ../README.md.
