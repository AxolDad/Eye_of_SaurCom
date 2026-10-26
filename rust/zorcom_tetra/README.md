# zorcom_tetra

TETRA network information layer (ETSI EN 300 392-2): only what every cell broadcasts in the clear to all terminals. SYNC (BSCH) with D-MLE-SYNC (system code, colour code,
timeslot / frame / multiframe, sharing mode, MCC, MNC), SYSINFO (BNCH) with D-MLE-SYSINFO (carrier, band, offset, duplex spacing, access parameters, location area, BS service
details, security class), BS service text, downlink / uplink carrier maths, D-NWRK-BROADCAST (neighbour cells, network time and its text), the ACCESS-ASSIGN slot usage and a
signalling block walk that reads the broadcasts and only counts everything else by protocol discriminator (encrypted PDUs are only counted).

Out of scope by owner decision, and not present: voice, speech codec, call content, SDS / text messages, subscriber addresses (skipped, never kept).

Modules: `bits` (bit reader), `abi` (`repr(C)` records), `sysinfo`, `mac`; `ffi.rs` the only `unsafe`. No allocation: the block walk writes into caller buffers.
Stays in C++: the burst / PHY layers, receiver, engine, generator and the encoders (`encodeSync`, `encodeSysinfo`, `macNullPdu`, `macBroadcastResource`, `mleNwrkBroadcast`).
`tetra_pdu_rs.cpp` is the Rust-backed implementation of the parse side of `dect2/tetra/tetra_pdu.h`; `tetra_pdu.cpp` keeps the encoders and generator PDUs.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/tetra.h`, symbols prefixed `zorcom_tetra_`. Golden-vector test: `tests/tetra/test_tetra_proto_rs.cpp` (2 M results hashed at the
default 200000 iterations), fuzz target `fuzz_tetra_proto`. Conventions: ../README.md.
