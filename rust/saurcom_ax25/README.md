# saurcom_ax25

AX.25 v2.2 and APRS 1.0.1: the frame parser (addresses, digipeater path, control, PID), the FCS (CRC-16/X-25), the text forms (`CALL-SSID`, `WIDE1-1*,WIDE2-1`, the TNC-2
line `FROM>TO,PATH:info`, frame type names), the HDLC receiver state machine (flags, bit unstuffing, abort, FCS check, frame hand-out), and the APRS information field parser:
uncompressed and compressed positions, Mic-E, objects, items, status, messages with ack / reject, weather (also positionless), telemetry, raw GPS, capabilities, queries,
third-party traffic, with the one-line summaries and latitude / longitude text.

Modules: `frame`, `hdlc`, `aprs` (dispatch and summaries), `aprs_pos` (positions, Mic-E), `aprs_wx` (weather), `arena` (text results in a caller buffer, no allocation). `unsafe`
only in `ffi.rs`, which also holds `cfn`: C `pow` and `snprintf("%.Nf")`, so the numbers in the text and the altitude / speed formulas match the C++ reference bit for bit.

Stays in C++ (`core/src/packet/packet_ax25.cpp`, built in every build): `buildFrame`, `appendFcs`, `hdlcBits`, `nrziEncode`, `scramble` (transmitter side, used by the test
signal and the generator) and the demodulator / receiver. `HdlcRx` itself is a thin C++ class around the shared `HdlcState` struct. `packet_ax25_rs.cpp` and
`packet_aprs_rs.cpp` are the Rust-backed implementations of the rest of `dect2/packet/packet_ax25.h` and `packet_aprs.h`. `packet_aprs.cpp` is compiled with
`-ffp-contract=off -fno-builtin-sin -fno-builtin-cos` (see ../README.md, floating point). The LoRa APRS text in `core/src/mesh/mesh_proto_aprs.cpp` belongs to the mesh module.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/ax25.h`, symbols prefixed `saurcom_ax25_` (APRS: `saurcom_ax25_aprs_`). Golden-vector test: `tests/packet/test_packet_proto_rs.cpp` (about
54 M results hashed), fuzz target `fuzz_packet_proto`. Status and conventions: ../README.md.
