# zorcom_hfdl

HFDL (HF data link, ARINC 635 / ICAO Doc 9741) layers above the modem: the eight burst rates and known symbols, the 15-bit m-sequence scrambler, the 40-row block
interleaver, the rate 1/2 K = 7 convolutional code (generators 0x6D first, then 0x4F: PR #175, checked on air) with a soft Viterbi decoder, the FCS (CRC-16/X.25),
the ground station table with frequency masks, and the PDU parser (SPDU squitters, MPDU headers up and down, LPDUs, HFNPDUs: performance data, frequency data,
system table; aircraft positions). The ACARS block of an HFNPDU is only located here (`acars_at`); the C++ wrapper decodes it with the ACARS and aero code.

Stays in C++ (`core/src/hfdl/`): the PSK maps (they use libm sin/cos/atan2), the burst and PDU builders for the test signal, the ACARS reader, the demodulator, the receiver.
The C++ of the converted functions is deleted; `tests/hfdl/test_hfdl_proto_rs.cpp` is the golden-vector test.

No allocation: the Viterbi survivors and the interleaver scratch are caller buffers; text goes through `zorcom_common::text::Buf` with the C library `snprintf` for the
`%.4f` / `%.3f` digits (so they equal the C++). Everything compared is integer, byte or text; the only floats are the Viterbi metrics (plain adds, same order, no FMA).

Licence: written from the ARINC 635 / ICAO Doc 9741 facts and from Eye of ZorCom's own C++; dumphfdl (szpajder, GPL) was read for reference only, no code copied.
C ABI: `src/ffi.rs`, header `core/include/dect2/rs/hfdl.h`, symbols prefixed `zorcom_hfdl_`.
