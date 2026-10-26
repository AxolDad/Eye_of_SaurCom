# zorcom_uat

UAT 978 MHz message layer (RTCA DO-282B, FIS-B APDU header per RTCA DO-358B), a port of dump978's `uat_decode.c` decoding half:

* `downlink`: the ADS-B / TIS-B payload (18 / 34 bytes): header (message type 0..31, address qualifier, 24 bit address), state vector (NIC, 24 bit position, barometric / geometric
  altitude, air / ground state, subsonic / supersonic velocity components, speed and track with `atan2` / `sqrt` from the C library, ground speed, track / heading, aircraft size, UTC coupling,
  TIS-B site), mode status (base-40 call sign or squawk, emitter category, emergency, version, SIL, NACp / NACv, capability flags) and the auxiliary state vector (secondary altitude).
* `uplink`: the uplink frame (position, slot, TIS-B site, UTC coupling, the 424 byte application data split into information frames, FIS-B APDU flags / product id / four time formats).

Results are flat `repr(C)` records (no bit fields). Reads past the end of a frame give 0; nothing here can panic. `unsafe` only in `ffi.rs` (which also holds the two libm functions).

Stays in C / C++: the Reed-Solomon FEC (`third_party/uat_fec`, `uat_fec_bridge.c`), the demodulator, tracker, generator and receiver, the display functions of `uat_decode.c` (text dump of a decoded message) and the
UI mapping `uatDecodeFrame` in `uat_proto.cpp`. `core/src/uat/uat_decode.c` keeps only dump978's display functions (the C decoding half is deleted); `uat_decode_rs.cpp` copies the Rust records into the C structs.

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/uat.h`, symbols prefixed `zorcom_uat_`. Golden-vector test: `tests/uat/test_uat_proto_rs.cpp` (digest recorded from dump978's C decoder, compiled a second time with the functions renamed `ref_*`;
200000 downlink and 200000 uplink frames per run, uplink also into pre-filled structs) and fuzz target `fuzz_uat_proto`. Status and conventions: ../README.md.
