// C ABI of the Rust crate rust/zorcom_dab (DAB data layers). Written by hand; the Rust test rust/zorcom/tests/headers.rs checks it against ffi.rs.
// Record arrays are (pointer, capacity); parsers return the number of entries found, counted
// past the capacity, SIZE_MAX (or false) when the input is too short. The record layouts are checked at start-up with zorcom_dab_struct_sizes().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZorcomDabFigRef { int32_t type; uint32_t off, len; } ZorcomDabFigRef;
typedef struct ZorcomDabFigHead { int32_t cn, oe, pd, ext; } ZorcomDabFigHead;
typedef struct ZorcomDabFig01Sub { int32_t id, start; uint8_t eep, uep_table; int32_t option, level, size, uep_index; } ZorcomDabFig01Sub;
typedef struct ZorcomDabFig03Packet { int32_t scid; uint8_t dg; int32_t dscty, sub_id, address; } ZorcomDabFig03Packet;
typedef struct ZorcomDabFig02Service { uint32_t sid, first, ncomp; } ZorcomDabFig02Service;
typedef struct ZorcomDabFig02Comp { int32_t tmid, ascty, dscty, sub_id, scid; uint8_t primary; } ZorcomDabFig02Comp;
typedef struct ZorcomDabFig013App { uint32_t sid; int32_t scids, index, type; uint32_t off, len; } ZorcomDabFig013App;
typedef struct ZorcomDabFig017Pty { uint32_t sid; uint8_t sd, primary; int32_t lang, pty; } ZorcomDabFig017Pty;
typedef struct ZorcomDabFig018Support { uint32_t sid, asu, first, ncl; } ZorcomDabFig018Support;
typedef struct ZorcomDabFig019Switch { int32_t cluster; uint32_t asw; uint8_t new_flag, region; int32_t sub_id; } ZorcomDabFig019Switch;
typedef struct ZorcomDabFig010Time { uint32_t mjd; int32_t hour, minute, second; int64_t utc; } ZorcomDabFig010Time;
typedef struct ZorcomDabFig1Label { int32_t ext, charset, scids; uint32_t sid, off; } ZorcomDabFig1Label;
typedef struct ZorcomDabFig2Seg { int32_t ext, toggle, seg, rfu, scids; uint32_t sid; uint8_t ucs2; int32_t total; uint8_t valid; uint32_t text_off, text_len; } ZorcomDabFig2Seg;
typedef struct ZorcomDabMotTime { uint8_t valid, now, long_form; int32_t mjd, hour, minute, second, ms; } ZorcomDabMotTime;
typedef struct ZorcomDabRange { uint32_t off, len; } ZorcomDabRange;
typedef struct ZorcomDabMotHeader {
    uint64_t body_size, header_size; int32_t content_type, subtype; uint32_t n_params; uint8_t has_name, has_slide, has_trigger, has_expire;
    int32_t category_id, slide_id; ZorcomDabRange name, category, click_url, alt_url; uint32_t caption_len; ZorcomDabMotTime trigger, expire;
} ZorcomDabMotHeader;
typedef struct ZorcomDabMotParam { int32_t id; ZorcomDabRange data; } ZorcomDabMotParam;
// status: 0 ignored, 1 bad CRC or structure, 2 scrambled body, 3 compressed directory, 4 segment (type, seg_no, tid, off / len of the data)
typedef struct ZorcomDabMotGroup { uint8_t status, last; int32_t type, seg_no, tid; uint32_t off, len; } ZorcomDabMotGroup;
typedef struct ZorcomDabDirEntry { int32_t tid; uint32_t off, size; } ZorcomDabDirEntry;
typedef struct ZorcomDabPadBytes { uint32_t xlen; uint8_t fpad[2]; } ZorcomDabPadBytes;
typedef struct ZorcomDabPadSub { int32_t type; uint32_t off, len; } ZorcomDabPadSub;
typedef struct ZorcomDabDlsStep { uint8_t action; uint32_t seg_len; } ZorcomDabDlsStep;
typedef struct ZorcomDabDlSegment { int32_t toggle; uint8_t first, last, cmd; int32_t code, seg_no, charset; } ZorcomDabDlSegment;
typedef struct ZorcomDabDlpTag { int32_t type, start, len; } ZorcomDabDlpTag;
typedef struct ZorcomDabJlGroup { uint8_t status; int32_t type; uint32_t field_len; } ZorcomDabJlGroup;
typedef struct ZorcomDabTocEntry { uint32_t id, desc; } ZorcomDabTocEntry;
typedef struct ZorcomDabTocInfo { int32_t revision, block_count, block_index, object_count, timeout_min; uint32_t n; } ZorcomDabTocInfo;
typedef struct ZorcomDabPacket { uint8_t crc_ok, first, last, cmd; int32_t ci, addr; uint32_t ul; } ZorcomDabPacket;
typedef struct ZorcomDabDrmPacket { uint8_t crc_ok, first, last, len_bad; int32_t id, ci; uint32_t n, at; } ZorcomDabDrmPacket;

// ABI version of this module (1).
uint32_t zorcom_dab_version(void);
// sizeof of the structs above in the order FigRef, FigHead, Fig01Sub, Fig03Packet, Fig02Service, Fig02Comp, Fig013App, Fig017Pty, Fig018Support, Fig019Switch,
// Fig010Time, Fig1Label, Fig2Seg, MotTime, MotHeader, MotParam, MotGroup, DirEntry, PadBytes, PadSub, DlsStep, DlSegment, DlpTag, JlGroup, TocEntry, TocInfo, Packet, DrmPacket.
void zorcom_dab_struct_sizes(size_t out[28]);

uint16_t zorcom_dab_crc16(const uint8_t* p, size_t n);   // CRC-CCITT, start 0xFFFF, inverted

// FIB (30 data bytes) and FIGs.
size_t zorcom_dab_fib_walk(const uint8_t* fib, size_t n, ZorcomDabFigRef* out, size_t cap, uint32_t* used);
void zorcom_dab_fig_head(int32_t type, const uint8_t* d, size_t n, ZorcomDabFigHead* out);
bool zorcom_dab_fig0_eid(const uint8_t* d, size_t n, int32_t* eid);
size_t zorcom_dab_fig0_subchannels(const uint8_t* d, size_t n, ZorcomDabFig01Sub* out, size_t cap);
size_t zorcom_dab_fig0_packet(const uint8_t* d, size_t n, ZorcomDabFig03Packet* out, size_t cap);
size_t zorcom_dab_fig0_services(const uint8_t* d, size_t n, ZorcomDabFig02Service* svcs, size_t scap, ZorcomDabFig02Comp* comps, size_t ccap, size_t* ncomp);
size_t zorcom_dab_fig0_userapps(const uint8_t* d, size_t n, ZorcomDabFig013App* out, size_t cap);
size_t zorcom_dab_fig0_pty(const uint8_t* d, size_t n, ZorcomDabFig017Pty* out, size_t cap);
size_t zorcom_dab_fig0_ann_support(const uint8_t* d, size_t n, ZorcomDabFig018Support* out, size_t cap, uint8_t* clusters, size_t ccap, size_t* ncl);
size_t zorcom_dab_fig0_ann_switch(const uint8_t* d, size_t n, ZorcomDabFig019Switch* out, size_t cap);
bool zorcom_dab_fig0_time(const uint8_t* d, size_t n, ZorcomDabFig010Time* out);
bool zorcom_dab_fig1(const uint8_t* d, size_t n, ZorcomDabFig1Label* out);
bool zorcom_dab_fig2(const uint8_t* d, size_t n, ZorcomDabFig2Seg* out);

// MOT.
size_t zorcom_dab_mot_parse_time(const uint8_t* d, size_t n, ZorcomDabMotTime* t);
size_t zorcom_dab_mot_time_text(const ZorcomDabMotTime* t, uint8_t* out, size_t cap);   // 64 bytes are enough
bool zorcom_dab_mot_parse_header(const uint8_t* h, size_t n, ZorcomDabMotHeader* out, ZorcomDabMotParam* params, size_t pcap, uint8_t* caption, size_t ccap);   // caption: 3 bytes per header byte
size_t zorcom_dab_mot_content_type_name(int32_t type, int32_t sub, uint8_t* out, size_t cap);   // 24 bytes are enough
void zorcom_dab_mot_parse_group(const uint8_t* d, size_t n, ZorcomDabMotGroup* out);
size_t zorcom_dab_mot_parse_directory(const uint8_t* d, size_t n, ZorcomDabDirEntry* out, size_t cap);   // SIZE_MAX: shorter than 13 bytes

// PAD.
bool zorcom_dab_pad_from_au(const uint8_t* au, size_t n, uint8_t* xpad, size_t cap, ZorcomDabPadBytes* out);   // xpad: 508 bytes
bool zorcom_dab_pad_from_mp2(const uint8_t* f, size_t n, int32_t* last_cont, uint8_t* xpad, size_t cap, ZorcomDabPadBytes* out);   // xpad: 196 bytes
size_t zorcom_dab_pad_split(const uint8_t* xp, size_t n, const uint8_t* fpad2, int32_t* last_type, ZorcomDabPadSub* out, size_t cap);   // out: 5 entries
uint8_t zorcom_dab_pad_dg_length(const uint8_t* d, size_t n, int32_t* len);
void zorcom_dab_pad_dls_step(const uint8_t* d, size_t n, ZorcomDabDlsStep* out);
void zorcom_dab_pad_dl_segment(const uint8_t* d, size_t n, ZorcomDabDlSegment* out);
size_t zorcom_dab_pad_dlplus_tags(const uint8_t* d, size_t n, ZorcomDabDlpTag* out4, uint8_t* running);   // SIZE_MAX: too short

// Journaline framing and packet mode.
void zorcom_dab_jl_data_group(const uint8_t* d, size_t n, ZorcomDabJlGroup* out);
bool zorcom_dab_jl_parse_toc(const uint8_t* d, size_t n, ZorcomDabTocInfo* info, ZorcomDabTocEntry* out, size_t cap);
bool zorcom_dab_jl_user_app_data(const uint8_t* d, size_t n, int32_t* version, int32_t* ext_header, int32_t* entry);
void zorcom_dab_packet(const uint8_t* p, size_t n, ZorcomDabPacket* out);
void zorcom_dab_drm_packet(const uint8_t* p, size_t n, ZorcomDabDrmPacket* out);

#ifdef __cplusplus
}
#endif
