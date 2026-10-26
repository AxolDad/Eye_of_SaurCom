# zorcom_wisun

Wi-SUN FAN (smart meters, street lights) over the IEEE 802.15.4g SUN FSK PHY, receive only, no keys:

* `phy`: the 2-octet PHR (mode switch, FCS type, whitening, 11 bit length), PN9 data whitening (x^9 + x^5 + 1, the table of 2048 octets repeats as the reference does), the 2-octet FCS (CRC-16/KERMIT)
  and the 4-octet FCS (CRC-32), FCS check.
* `mac`: the 802.15.4-2015 MAC header (frame version 2 PAN ID rules of table 7-2, the 2006 rules before it, address modes, auxiliary security header with key id mode), header IEs (WH-IE: UTT, BT, FC, RSL, EA,
  LBC, LFN sub-IDs, HT1 / HT2 terminators), payload IEs of frames that are not secured, and the one-line summary; secured frames keep their payload encrypted. `ie`: US-IE / BS-IE schedules and their channel
  information (plans 0 / 1 / 2, channel functions, excluded ranges and masks), WP-IE sub-IEs (PAN, NETNAME, PANVER, GTKHASH, POM, LFNVER), the MPX IE with the EAPOL / EAP summary.
* `names`: UTT frame types, channel functions, regulatory domains, 802.15.4 frame types, the schedule text.

Results are flat `repr(C)` records; text is length plus bytes (the IE list holds 8192 bytes, more than the IE names of a 2047 octet PSDU need; POM keeps up to 2048 PHY mode ids). `unsafe` only in `ffi.rs`, which
also holds `cfn` (the C library `snprintf("%.3f")` for explicit channel plan frequencies). The name functions return pointers to static NUL-terminated text.

Stays in C++ (`core/src/wisun/`): the FSK demodulator, frame sync, receiver, generator and engine glue, the band and channel plan tables and `channelOf` / `channelUsable`, the MPDU builder (`buildMpdu`), `addFcs`,
and the text helpers `srcText` / `dstText` / `euiText` / `frameTypeName` that call the 2.4 GHz mode (`zigbee::`). The C++ of the converted functions is deleted; `wisun_proto_rs.cpp` is the
Rust-backed implementation (layout check at start-up).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/wisun.h`, symbols prefixed `zorcom_wisun_`. Golden-vector test: `tests/wisun/test_wisun_proto_rs.cpp` (digest recorded from the C++ it replaced) (every PHR value, whitening
past the 2048 octet wrap, FCS, names, 200000 schedule texts, 200000 frames built / random-structured / mutated: 1.1 M results hashed), fuzz target `fuzz_wisun_proto`. Status and conventions: ../README.md.
