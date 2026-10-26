# zorcom_drm

The parsing side of the DRM signalling and data layers (ETSI ES 201 980; the DRM+ mode uses the same FAC / SDC framing):

* **FAC** (`fac`): the 72 bit (116 bit in robustness mode E) block with its CRC-8: channel parameters (occupancy, interleaver, MSC / SDC mode, number of services, reconfiguration index) and the service parameters of
  one service (two in mode E): identity, short id, conditional access flags, language, audio / data flag and programme type / application id.
* **SDC** (`sdc`): the data field entities (multiplex description current and announced, labels, application information incl. packet mode parameters, time and date with local offset, audio information incl. the
  xHE-AAC static configuration, language and country) parsed in place on a record that carries the previous state, exactly like the C++ `SdcInfo` (entities only touch what they carry), and the CRC-16 check of a whole SDC
  block with its AFS index.
* **Audio super frame** (`aac`): frames per super frame, the frame border header, and the split of a logical frame into audio frames and their CRC bytes (higher protected part first).
* **Text message** (`text`): the bytes a segment needs and the CRC / header check of a segment (the 4 byte unit assembly and the message store stay in C++).
* **Tables and helpers** (`names`, `data`): language, programme type, MSC mode, occupancy and application names, service counts, the MJD calendar conversion, the payload of an MSC data group, the text of an object for
  display (control characters out, ISO 8859-1 to UTF-8 when the bytes are not UTF-8) and the "looks like text" heuristic. `bits`: the CRC-8 / CRC-16 over unpacked bits and a bit reader.

MOT (objects, directories, data groups) and the packet mode headers are in `zorcom_dab` (shared with DAB); the CRC-16 of the byte oriented DRM layers is the same CRC-GENIBUS, taken from `zorcom_dab::crc`.

`unsafe` only in `ffi.rs`; plain `repr(C)` records (fixed 128 byte fields for the SDC strings, an entity body is at most 127 bytes), no allocation. Behaviour fixed on the way (identically in the C++ reference `drm_proto.cpp`
and in Rust): the audio super frame parser refuses a higher protected part that does not fit the logical frame (it used to read past the end) and more than 16 frames.

Stays in C++: the demodulator and channel coding (`drm_rx.cpp`, `drm_fec.cpp`, `drm_defs.cpp`, OFDM / pilots), the generators and builders (`drm_gen*.cpp`, `facBuild`, `sdcEntity*`, `TextEncoder`, `aacSuperFrameBuild`), the AAC / xHE-AAC
decoders, the text message assembly, the data service state with MOT slide show / website / TPEG handling (`DrmDataService`), and the Journaline objects (they inflate).

`drm_proto.h` is the C++ / Rust boundary (namespace `dect2::drmp`, the records are those of `drm_msg.h`); `drm_proto_rs.cpp` implements it (the C++ copy is deleted).
Golden-vector test: `tests/drm/test_drm_proto_rs.cpp` (13 M results hashed at the default 200000 iterations per family), fuzz target `fuzz_drm_proto`. C ABI: `src/ffi.rs`, header `core/include/dect2/rs/drm.h`, symbols prefixed
`zorcom_drm_`. Status and conventions: ../README.md.
