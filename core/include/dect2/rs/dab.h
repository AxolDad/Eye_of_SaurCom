// C ABI of the Rust crate rust/saurcom_dab (DAB data layers). Written by hand; the Rust test rust/saurcom/tests/headers.rs checks it against ffi.rs.
// Record arrays are (pointer, capacity); parsers return the number of entries found, counted
// past the capacity, SIZE_MAX (or false) when the input is too short. The record layouts are checked at start-up with saurcom_dab_struct_sizes().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaurcomDabFigRef { int32_t type; uint32_t off, len; } SaurcomDabFigRef;
typedef struct SaurcomDabFigHead { int32_t cn, oe, pd, ext; } SaurcomDabFigHead;
typedef struct SaurcomDabFig01Sub { int32_t id, start; uint8_t eep, uep_table; int32_t option, level, size, uep_index; } SaurcomDabFig01Sub;
typedef struct SaurcomDabFig03Packet { int32_t scid; uint8_t dg; int32_t dscty, sub_id, address; } SaurcomDabFig03Packet;
typedef struct SaurcomDabFig02Service { uint32_t sid, first, ncomp; } SaurcomDabFig02Service;
typedef struct SaurcomDabFig02Comp { int32_t tmid, ascty, dscty, sub_id, scid; uint8_t primary; } SaurcomDabFig02Comp;
typedef struct SaurcomDabFig013App { uint32_t sid; int32_t scids, index, type; uint32_t off, len; } SaurcomDabFig013App;
typedef struct SaurcomDabFig017Pty { uint32_t sid; uint8_t sd, primary; int32_t lang, pty; } SaurcomDabFig017Pty;
typedef struct SaurcomDabFig018Support { uint32_t sid, asu, first, ncl; } SaurcomDabFig018Support;
typedef struct SaurcomDabFig019Switch { int32_t cluster; uint32_t asw; uint8_t new_flag, region; int32_t sub_id; } SaurcomDabFig019Switch;
typedef struct SaurcomDabFig010Time { uint32_t mjd; int32_t hour, minute, second; int64_t utc; } SaurcomDabFig010Time;
typedef struct SaurcomDabFig1Label { int32_t ext, charset, scids; uint32_t sid, off; } SaurcomDabFig1Label;
typedef struct SaurcomDabFig2Seg { int32_t ext, toggle, seg, rfu, scids; uint32_t sid; uint8_t ucs2; int32_t total; uint8_t valid; uint32_t text_off, text_len; } SaurcomDabFig2Seg;
typedef struct SaurcomDabMotTime { uint8_t valid, now, long_form; int32_t mjd, hour, minute, second, ms; } SaurcomDabMotTime;
typedef struct SaurcomDabRange { uint32_t off, len; } SaurcomDabRange;
typedef struct SaurcomDabMotHeader {
    uint64_t body_size, header_size; int32_t content_type, subtype; uint32_t n_params; uint8_t has_name, has_slide, has_trigger, has_expire;
    int32_t category_id, slide_id; SaurcomDabRange name, category, click_url, alt_url; uint32_t caption_len; SaurcomDabMotTime trigger, expire;
} SaurcomDabMotHeader;
typedef struct SaurcomDabMotParam { int32_t id; SaurcomDabRange data; } SaurcomDabMotParam;
// status: 0 ignored, 1 bad CRC or structure, 2 scrambled body, 3 compressed directory, 4 segment (type, seg_no, tid, off / len of the data)
typedef struct SaurcomDabMotGroup { uint8_t status, last; int32_t type, seg_no, tid; uint32_t off, len; } SaurcomDabMotGroup;
typedef struct SaurcomDabDirEntry { int32_t tid; uint32_t off, size; } SaurcomDabDirEntry;
typedef struct SaurcomDabPadBytes { uint32_t xlen; uint8_t fpad[2]; } SaurcomDabPadBytes;
typedef struct SaurcomDabPadSub { int32_t type; uint32_t off, len; } SaurcomDabPadSub;
typedef struct SaurcomDabDlsStep { uint8_t action; uint32_t seg_len; } SaurcomDabDlsStep;
typedef struct SaurcomDabDlSegment { int32_t toggle; uint8_t first, last, cmd; int32_t code, seg_no, charset; } SaurcomDabDlSegment;
typedef struct SaurcomDabDlpTag { int32_t type, start, len; } SaurcomDabDlpTag;
typedef struct SaurcomDabJlGroup { uint8_t status; int32_t type; uint32_t field_len; } SaurcomDabJlGroup;
typedef struct SaurcomDabTocEntry { uint32_t id, desc; } SaurcomDabTocEntry;
typedef struct SaurcomDabTocInfo { int32_t revision, block_count, block_index, object_count, timeout_min; uint32_t n; } SaurcomDabTocInfo;
typedef struct SaurcomDabPacket { uint8_t crc_ok, first, last, cmd; int32_t ci, addr; uint32_t ul; } SaurcomDabPacket;
typedef struct SaurcomDabDrmPacket { uint8_t crc_ok, first, last, len_bad; int32_t id, ci; uint32_t n, at; } SaurcomDabDrmPacket;

// ABI version of this module (1).
uint32_t saurcom_dab_version(void);
// sizeof of the structs above in the order FigRef, FigHead, Fig01Sub, Fig03Packet, Fig02Service, Fig02Comp, Fig013App, Fig017Pty, Fig018Support, Fig019Switch,
// Fig010Time, Fig1Label, Fig2Seg, MotTime, MotHeader, MotParam, MotGroup, DirEntry, PadBytes, PadSub, DlsStep, DlSegment, DlpTag, JlGroup, TocEntry, TocInfo, Packet, DrmPacket.
void saurcom_dab_struct_sizes(size_t out[28]);

uint16_t saurcom_dab_crc16(const uint8_t* p, size_t n);   // CRC-CCITT, start 0xFFFF, inverted

// FIB (30 data bytes) and FIGs.
size_t saurcom_dab_fib_walk(const uint8_t* fib, size_t n, SaurcomDabFigRef* out, size_t cap, uint32_t* used);
void saurcom_dab_fig_head(int32_t type, const uint8_t* d, size_t n, SaurcomDabFigHead* out);
bool saurcom_dab_fig0_eid(const uint8_t* d, size_t n, int32_t* eid);
size_t saurcom_dab_fig0_subchannels(const uint8_t* d, size_t n, SaurcomDabFig01Sub* out, size_t cap);
size_t saurcom_dab_fig0_packet(const uint8_t* d, size_t n, SaurcomDabFig03Packet* out, size_t cap);
size_t saurcom_dab_fig0_services(const uint8_t* d, size_t n, SaurcomDabFig02Service* svcs, size_t scap, SaurcomDabFig02Comp* comps, size_t ccap, size_t* ncomp);
size_t saurcom_dab_fig0_userapps(const uint8_t* d, size_t n, SaurcomDabFig013App* out, size_t cap);
size_t saurcom_dab_fig0_pty(const uint8_t* d, size_t n, SaurcomDabFig017Pty* out, size_t cap);
size_t saurcom_dab_fig0_ann_support(const uint8_t* d, size_t n, SaurcomDabFig018Support* out, size_t cap, uint8_t* clusters, size_t ccap, size_t* ncl);
size_t saurcom_dab_fig0_ann_switch(const uint8_t* d, size_t n, SaurcomDabFig019Switch* out, size_t cap);
bool saurcom_dab_fig0_time(const uint8_t* d, size_t n, SaurcomDabFig010Time* out);
bool saurcom_dab_fig1(const uint8_t* d, size_t n, SaurcomDabFig1Label* out);
bool saurcom_dab_fig2(const uint8_t* d, size_t n, SaurcomDabFig2Seg* out);

// MOT.
size_t saurcom_dab_mot_parse_time(const uint8_t* d, size_t n, SaurcomDabMotTime* t);
size_t saurcom_dab_mot_time_text(const SaurcomDabMotTime* t, uint8_t* out, size_t cap);   // 64 bytes are enough
bool saurcom_dab_mot_parse_header(const uint8_t* h, size_t n, SaurcomDabMotHeader* out, SaurcomDabMotParam* params, size_t pcap, uint8_t* caption, size_t ccap);   // caption: 3 bytes per header byte
size_t saurcom_dab_mot_content_type_name(int32_t type, int32_t sub, uint8_t* out, size_t cap);   // 24 bytes are enough
void saurcom_dab_mot_parse_group(const uint8_t* d, size_t n, SaurcomDabMotGroup* out);
size_t saurcom_dab_mot_parse_directory(const uint8_t* d, size_t n, SaurcomDabDirEntry* out, size_t cap);   // SIZE_MAX: shorter than 13 bytes

// PAD.
bool saurcom_dab_pad_from_au(const uint8_t* au, size_t n, uint8_t* xpad, size_t cap, SaurcomDabPadBytes* out);   // xpad: 508 bytes
bool saurcom_dab_pad_from_mp2(const uint8_t* f, size_t n, int32_t* last_cont, uint8_t* xpad, size_t cap, SaurcomDabPadBytes* out);   // xpad: 196 bytes
size_t saurcom_dab_pad_split(const uint8_t* xp, size_t n, const uint8_t* fpad2, int32_t* last_type, SaurcomDabPadSub* out, size_t cap);   // out: 5 entries
uint8_t saurcom_dab_pad_dg_length(const uint8_t* d, size_t n, int32_t* len);
void saurcom_dab_pad_dls_step(const uint8_t* d, size_t n, SaurcomDabDlsStep* out);
void saurcom_dab_pad_dl_segment(const uint8_t* d, size_t n, SaurcomDabDlSegment* out);
size_t saurcom_dab_pad_dlplus_tags(const uint8_t* d, size_t n, SaurcomDabDlpTag* out4, uint8_t* running);   // SIZE_MAX: too short

// Journaline framing and packet mode.
void saurcom_dab_jl_data_group(const uint8_t* d, size_t n, SaurcomDabJlGroup* out);
bool saurcom_dab_jl_parse_toc(const uint8_t* d, size_t n, SaurcomDabTocInfo* info, SaurcomDabTocEntry* out, size_t cap);
bool saurcom_dab_jl_user_app_data(const uint8_t* d, size_t n, int32_t* version, int32_t* ext_header, int32_t* entry);
void saurcom_dab_packet(const uint8_t* p, size_t n, SaurcomDabPacket* out);
void saurcom_dab_drm_packet(const uint8_t* p, size_t n, SaurcomDabDrmPacket* out);

#ifdef __cplusplus
}
#endif
