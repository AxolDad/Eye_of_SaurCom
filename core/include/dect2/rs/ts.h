// C ABI of the Rust crate rust/saurcom_ts (MPEG-TS packet header, PSI / SI section parsers). Written by hand; the Rust test rust/saurcom/tests/headers.rs checks
// it against ffi.rs. Text fields are byte ranges (SaurcomTsSpan) of the section.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaurcomTsSpan { uint32_t off, len; } SaurcomTsSpan;
typedef struct SaurcomTsPacketHeader { int32_t pid; uint8_t tei, pusi; int32_t scr, afc, cc, payload_off; } SaurcomTsPacketHeader;
typedef struct SaurcomTsPatEntry { int32_t prog, pid; } SaurcomTsPatEntry;
// kind: 0 data, 1 video, 2 audio, 3 subtitles, 4 teletext.
typedef struct SaurcomTsPmtStream {
    int32_t stream_type, pid; uint8_t kind, codec_len, has_lang, lang[3], codec[24]; uint32_t sub_first, sub_count;
} SaurcomTsPmtStream;
typedef struct SaurcomTsPmtSub { uint8_t teletext, lang[3]; int32_t type, page, anc_page; } SaurcomTsPmtSub;
typedef struct SaurcomTsPmtInfo { int32_t prog, pcr_pid; uint8_t ca; uint32_t n_streams, n_subs; } SaurcomTsPmtInfo;
typedef struct SaurcomTsSdtService {
    int32_t sid; uint8_t free_ca, has_type, has_provider, has_name; int32_t type; SaurcomTsSpan provider, name;
} SaurcomTsSdtService;
typedef struct SaurcomTsNitLcn { int32_t sid, lcn; } SaurcomTsNitLcn;
typedef struct SaurcomTsNitInfo { uint8_t has_name; SaurcomTsSpan name; uint32_t n_lcn; } SaurcomTsNitInfo;
typedef struct SaurcomTsTvct { uint8_t name[8], name_len; int32_t major, minor, prog, stype; } SaurcomTsTvct;
typedef struct SaurcomTsTdt { int64_t unix_s; uint8_t text_len, text[32]; } SaurcomTsTdt;
typedef struct SaurcomTsEitEvent {
    int32_t event_id; int64_t start; int32_t duration, running, genre; uint8_t lang[3], has_lang, has_title, has_text; uint16_t ext_mask;
    SaurcomTsSpan title, text, ext[16];
} SaurcomTsEitEvent;
typedef struct SaurcomTsEitInfo { int32_t sid, sec_num; uint8_t legacy, has_name; SaurcomTsSpan now_next_name; uint32_t n_events; } SaurcomTsEitInfo;

// ABI version of this module (1).
uint32_t saurcom_ts_version(void);
// sizeof of the structs above in the order PacketHeader, PatEntry, PmtStream, PmtSub, PmtInfo, SdtService, NitLcn, NitInfo, Tvct, Tdt, EitEvent, EitInfo.
void saurcom_ts_struct_sizes(size_t out[12]);

// MPEG-2 section CRC-32 (polynomial 0x04C11DB7, start 0xFFFFFFFF): 0 over a section that includes its CRC.
uint32_t saurcom_ts_crc32(const uint8_t* p, size_t n);
// Header of a transport packet (n >= 4 bytes, sync 0x47); false otherwise. payload_off is -1 when there is no payload.
bool saurcom_ts_packet_header(const uint8_t* p, size_t n, SaurcomTsPacketHeader* out);
// The 188 byte PAT packet naming one program (sid at pmt_pid) in transport stream tsid.
void saurcom_ts_pat_packet(int32_t tsid, int32_t sid, int32_t pmt_pid, int32_t cc, uint8_t* out188);

// Section parsers: the whole section (CRC checked by the caller). Record arrays are (pointer, capacity); the return is the number written (SIZE_MAX / false:
// the section is too short). Text fields are spans of the section.
size_t saurcom_ts_parse_pat(const uint8_t* s, size_t n, int32_t* tsid, SaurcomTsPatEntry* out, size_t cap);
bool saurcom_ts_parse_pmt(const uint8_t* s, size_t n, SaurcomTsPmtInfo* info, SaurcomTsPmtStream* streams, size_t scap, SaurcomTsPmtSub* subs, size_t bcap);
size_t saurcom_ts_parse_sdt(const uint8_t* s, size_t n, int32_t* tsid, int32_t* onid, SaurcomTsSdtService* out, size_t cap);
bool saurcom_ts_parse_nit(const uint8_t* s, size_t n, SaurcomTsNitInfo* info, SaurcomTsNitLcn* out, size_t cap);
size_t saurcom_ts_parse_tvct(const uint8_t* s, size_t n, int32_t* tsid, SaurcomTsTvct* out, size_t cap);
bool saurcom_ts_parse_tdt(const uint8_t* s, size_t n, SaurcomTsTdt* out);
bool saurcom_ts_parse_eit(const uint8_t* s, size_t n, SaurcomTsEitInfo* info, SaurcomTsEitEvent* out, size_t cap);

#ifdef __cplusplus
}
#endif
