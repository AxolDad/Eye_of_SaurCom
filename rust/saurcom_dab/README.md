# saurcom_dab

The DAB data layers (ETSI EN 300 401, TS 101 756, EN 301 234, TS 102 563, TS 102 980, TS 102 979), parsing only:

* **FIC** (`fig`): the FIB walk (30 data byte FIB, FIG type / length), the FIG header flags, FIG 0/0 (ensemble id), 0/1 (sub-channel organisation as sent: the size / bit rate tables are FEC data and stay in C++), 0/2 (services and
  components), 0/3 (packet mode components), 0/10 (date and time, MJD to Unix seconds), 0/13 (user applications), 0/17 (programme type, language), 0/18 and 0/19 (announcement support and switching), FIG 1 (labels: the byte
  range of the 16 label bytes and the character set field) and FIG 2 (extended label segments: identity, toggle, segment index, UCS-2 flag, text range).
* **MOT** (`mot`): MOT time and its text, the header core and header extension parameters (content name, caption with the DAB charset cleaned to UTF-8 through `saurcom_dvbsi::charset`, category / slide id, click-through and
  alternative URLs, trigger and expire times), the content type names, the MSC data group framing (CRC, extension / segmentation / user access headers, transport id, segment data range) and the directory walk. Shared with DRM.
* **PAD** (`pad`): the data stream element of a DAB+ access unit and the PAD of an MP2 frame (F-PAD / X-PAD, with the content-indicator state kept by the caller), the X-PAD subfield split, the data group length indicator, dynamic
  label segment framing (CRC-validated lengths of segments and commands), the segment prefix and the DL Plus tag command.
* **Journaline framing and packet mode** (`jl`): the restricted data group check, the TOC management block, the user application data of FIG 0/13, and the header fields (with CRC-16) of one DAB packet mode packet and one DRM packet
  mode packet.
* `crc`: the DAB CRC-16 (CCITT, start 0xFFFF, inverted).

`unsafe` only in `ffi.rs`; plain `repr(C)` records into caller arrays (`ffi_types.rs`), no allocation. Lists that cannot be longer than the input suggests are sized by the C++ wrapper from the input length.

Stays in C++: the ensemble, service and label tables with the FIG 2 segment store (`DabReceiver` in `dab_rx.cpp`), the sub-channel size / bit rate tables (`dab_fec.cpp`), the MOT object / directory reassembly (`MotDecoder`), the
data group, dynamic label and slide show assembly (`DabPadDecoder`, image decoding with FFmpeg), the SPI service information XML, the JML object parser (it inflates), the packet data unit reassembly, the FIG analyser text
decoder (`dab_fig*.cpp`, 1200 lines of etisnoop-style text for the analyser panel) and the DAB generator.

`dab_proto.h` is the C++ / Rust boundary (namespace `dect2::dabp`); `dab_proto_rs.cpp` implements it (the C++ copy is deleted). Golden-vector test:
`tests/dab/test_dab_proto_rs.cpp` (about 15 M results hashed at the default 200000 iterations per family), fuzz target `fuzz_dab_proto`. C ABI: `src/ffi.rs`, header `core/include/dect2/rs/dab.h`, symbols prefixed `saurcom_dab_`.
Status and conventions: ../README.md.
