# zorcom_ts

The pure parsing layer of the MPEG-2 transport stream analyser (ISO/IEC 13818-1, ETSI EN 300 468, ATSC A/65): the MPEG-2 section CRC-32 (polynomial 0x04C11DB7), the
188 byte packet header (PID, TEI, PUSI, scrambling, adaptation field control, continuity counter, payload offset), the single-program PAT packet the service filter sends, and the section parsers for
PAT, PMT (stream types to codec / kind, CA, language, DRA / AC-3 / E-AC-3 / DTS / AAC / DVB subtitling / teletext descriptors with their page lists), SDT (service descriptor 0x48), NIT (network name 0x40,
logical channel numbers 0x83), EIT (present / following name, every event with short event 0x4D, extended event 0x4E and content 0x54 descriptors, BCD start / duration), TDT / TOT (the MJD calendar
conversion of Annex C) and the ATSC TVCT / CVCT.

Modules: `crc`, `packet`, `si` (PAT PMT SDT NIT TVCT TDT), `eit`; `unsafe` only in `ffi.rs`. No allocation: parsers write plain `repr(C)` records into caller arrays (pointer + capacity). Text fields (service
and event names, network name) come back as byte ranges (`Span`) of the section: the character set conversion (`dvbText`, iconv for the ISO 8859 / GB 2312 / Big5 / EUC-KR tables) stays in C++, as does
every piece of state (section reassembly across packets, the service table, the EPG store with its 400 event bound, the bit-rate statistics). Two lists can be longer than the section length suggests when
corrupt length fields make descriptors overlap (PMT sub entries, NIT logical channel numbers): the parsers count every entry and the C++ wrapper parses again with a bigger array.

Behaviour fixed on the way (identically in the C++ reference `ts_proto.cpp` and in Rust): descriptor loops no longer read past the end of the section when a descriptor claims more bytes than are left (NIT
network name, NIT LCN list, the present / following name of an EIT); text spans are cut at the section end.

Stays in C++: `TsDemux` / `ServiceFilter` state and orchestration (`ts.cpp`), `dvbText` and iconv, the recorder and output (`ts_record.cpp`, `tsout.cpp`). `ts_proto_rs.cpp` is the Rust-backed
implementation of `dect2/tv/ts_proto.h` (the C++ copy is deleted).

C ABI: `src/ffi.rs`, header `core/include/dect2/rs/ts.h`, symbols prefixed `zorcom_ts_`. Golden-vector test: `tests/ts/test_ts_proto_rs.cpp` (5.5 M results hashed at the default 200000 iterations per
family), fuzz target `fuzz_ts_proto`. Status and conventions: ../README.md.
