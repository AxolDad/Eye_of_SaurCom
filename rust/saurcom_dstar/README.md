# saurcom_dstar

D-STAR digital voice (DV) frame layer (JARL D-STAR specification): the CRC-CCITT as D-STAR uses it (reflected 0x8408, start 0xFFFF, inverted), the 41 byte radio header (3 flag bytes, RPT2, RPT1, UR,
MY, MY suffix: pack with CRC, unpack with CRC check, printable-only callsign text), the flag text, the header FEC (K=3 convolutional code with generators 7 and 5, the 24-column interleaver over 660 bits, the
scrambler x^7 + x^4 + 1, a soft Viterbi decoder that reports the corrected channel bits), LSB-first byte / bit conversion, the AMBE bit order into mbelib's `ambe_fr[4][24]`, and the slow data GPS text:
NMEA `$GPRMC` / `$GPGGA` (any talker, checksum checked) and D-PRS lines (CRC checked, with or without the closing CR, time stamps, course / speed, `/A=` altitude, comment).

Modules: `header` (CRC, header fields, flag text, AMBE order), `fec`, `gps`; `unsafe` only in `ffi.rs`, which also holds `cfn`: the C library `atof`, `atoi`, `strtoul`, `lround` that the GPS parsers call so every
number is identical to the C++ reference bit for bit (no float parsing of our own). The parsers report whether the reference would have left the fix untouched (early checks) or written to it before failing,
and the C++ wrapper reproduces that. No allocation: fixed arrays (the Viterbi decoder takes up to 1024 bits; the header needs 330; GPS number fields are cut at 1023 bytes).

Stays in C++: the demodulator, frame / bit sync search, slow data assembly and call state (`dstar_rx.cpp`), the generator (`dstar_gen.cpp`), the sync pattern tables (`kFrameSync`, `kDataSync`, `kEndPattern`, `kSlowScramble`:
data the receiver uses) and the AMBE vocoder glue (`dstar_ambe.cpp`, mbelib untouched: the voice bits are only re-ordered here). `dstar_proto_rs.cpp` is the Rust-backed implementation of
`dect2/dstar/dstar_proto.h` (`dstar_proto.cpp` keeps only the sync tables).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/dstar.h`, symbols prefixed `saurcom_dstar_`. Golden-vector test: `tests/dstar/test_dstar_proto_rs.cpp` (2.4 M results hashed at the default 200000 iterations
per family; about 50000 NMEA and 40000 D-PRS lines parse, the rest are rejected the same way), fuzz target `fuzz_dstar_proto`. Status and conventions: ../README.md.
