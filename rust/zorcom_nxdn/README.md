# zorcom_nxdn

NXDN common air interface (NXDN TS 1-A / 1-D) frame layer: the PN9 scrambler, the LICH (RF channel type, functional channel type, option, direction, parity),
CRC-6 / CRC-12 / CRC-16 over bit arrays, the three coded channels (SACCH 26 bits, FACCH1 80 bits, CAC 155 bits: CRC, K=5 convolutional code G1 = 1 + D^3 + D^4 /
G2 = 1 + D + D^2 + D^4, puncturing, block interleaver, soft Viterbi decode with the channel bit error count), layer 3 messages (VCALL, TX_REL, STAT_REQ, SDCALL header and
data text, SITE_INFO, SRV_INFO: build and parse, names) and the AMBE+2 voice bit order into mbelib's `ambe_fr[4][24]`.

Modules: `lich` (scrambler, LICH), `crc`, `chan` (coded channels, fixed buffers, no allocation), `msg` (layer 3). `unsafe` only in `ffi.rs`.

Stays in C++: the demodulator, frame sync search, level calibration (floating point DSP), the SACCH superframe assembly and call state in `core/src/nxdn/nxdn_rx.cpp`, the
generator (`nxdn_gen.cpp`), the sync word table `kFsw` (data used by the receiver) and the mbelib vocoder (`nxdn_ambe.cpp`): the voice bits are only re-ordered here.
`core/src/nxdn/nxdn_proto_rs.cpp` is the Rust-backed implementation of `dect2/nxdn/nxdn_proto.h` (`nxdn_proto.cpp` keeps only the sync word table).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/nxdn.h`, symbols prefixed `zorcom_nxdn_`. Golden-vector test: `tests/nxdn/test_nxdn_proto_rs.cpp` (2.5 M results hashed at the default
200000 iterations per family, soft values include NaN and infinity), fuzz target `fuzz_nxdn_proto`. Status and conventions: ../README.md.
