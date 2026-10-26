// C ABI of the Rust crate rust/saurcom_ax25 (AX.25 / APRS). Written by hand; the Rust test rust/saurcom/tests/headers.rs checks it against ffi.rs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One address of a parsed frame (call: up to 6 characters without spaces)
typedef struct SaurcomAx25Addr {
    uint8_t call[8];
    uint8_t call_len;
    uint8_t h;              // H bit (digipeaters), command/response bit (source, destination)
    uint8_t pad[2];
    int32_t ssid;           // 0..15
} SaurcomAx25Addr;

// A parsed AX.25 frame; the information field is the input from info_off to its end.
typedef struct SaurcomAx25Frame {
    SaurcomAx25Addr to, from, path[8];
    uint32_t npath;
    int32_t control;
    int32_t pid;            // -1: none
    uint32_t pad;
    size_t info_off;
} SaurcomAx25Frame;

// An address handed in for text building.
typedef struct SaurcomAx25AddrIn {
    const uint8_t* call;
    size_t call_len;
    int32_t ssid;
    uint8_t h;
    uint8_t pad[3];
} SaurcomAx25AddrIn;

// A string inside the text buffer of saurcom_ax25_aprs_parse
typedef struct SaurcomSpan { uint32_t off, len; } SaurcomSpan;

// Result of saurcom_ax25_aprs_parse (mirrors the Rust Info struct; sizeof is checked at start-up with saurcom_ax25_struct_sizes). Strings are spans of the text buffer.
typedef struct SaurcomAprsInfo {
    int32_t kind;           // packet type id, see saurcom_ax25_aprs_type_name (0 = not set)
    int32_t course_deg;
    double lat, lon, alt_m, speed_knots;
    double wx[9];           // wind direction, wind, gust, temperature F, rain 1 h, rain 24 h, rain since midnight, humidity, pressure (NaN = not measured)
    SaurcomSpan name, comment, text, msg_no, mic_status, summary;
    uint8_t has_pos, has_alt, has_course, has_speed, live, is_ack, is_rej, has_wx;
    uint8_t sym_table, sym_code;
    uint8_t pad[6];
} SaurcomAprsInfo;

// State of the HDLC receiver (mirrors the Rust Hdlc struct and the C++ HdlcState)
typedef struct SaurcomAx25Hdlc {
    int32_t prev;
    uint8_t pat, acc;
    int32_t cnt;
    uint8_t in_frame, hunting, resync_clock;
    int32_t flag_recent;
    uint32_t len;
    uint8_t buf[402];
} SaurcomAx25Hdlc;

// ABI version of this module.
uint32_t saurcom_ax25_version(void);
// sizeof of the structs above as Rust sees them (frame, addr_in, info, hdlc).
void saurcom_ax25_struct_sizes(size_t* frame, size_t* addr_in, size_t* info, size_t* hdlc);

// ---- AX.25 frames
// Frame check sequence (CRC-16/X-25) of the bytes; true when the last two bytes are the FCS of the bytes before them.
uint16_t saurcom_ax25_fcs(const uint8_t* d, size_t n);
bool saurcom_ax25_fcs_ok(const uint8_t* d, size_t n);
// Parses a frame without FCS; false when malformed (out untouched).
bool saurcom_ax25_parse_frame(const uint8_t* d, size_t n, SaurcomAx25Frame* out);
// Text forms, written into out; they return the length or SIZE_MAX when the buffer is too small.
size_t saurcom_ax25_addr_str(const SaurcomAx25AddrIn* a, uint8_t* out, size_t cap);                                  // CALL or CALL-SSID
size_t saurcom_ax25_path_str(const SaurcomAx25AddrIn* path, size_t n, uint8_t* out, size_t cap);                     // WIDE1-1*,WIDE2-1
size_t saurcom_ax25_tnc2(const SaurcomAx25AddrIn* from, const SaurcomAx25AddrIn* to, const SaurcomAx25AddrIn* path, size_t n, const uint8_t* info,
                       size_t info_len, uint8_t* out, size_t cap);                                               // FROM>TO,PATH:info
size_t saurcom_ax25_type_name(uint32_t control, uint8_t* out, size_t cap);                                          // "UI", "RR", ...

// ---- HDLC receiver
void saurcom_ax25_hdlc_reset(SaurcomAx25Hdlc* h);
// One bit (NRZI level when nrzi): 0 nothing, 1 good frame (buf[0..*len) without FCS), 2 bad FCS (buf[0..*len) all bytes).
int32_t saurcom_ax25_hdlc_bit(SaurcomAx25Hdlc* h, int32_t level, uint32_t nrzi, size_t* len);

// ---- APRS
// Parses an information field: 1 true, 0 false, -1 text buffer too small. info_out is filled as far as the parser got; strings are spans of text.
int32_t saurcom_ax25_aprs_parse(const uint8_t* dest, size_t dest_len, const uint8_t* info, size_t info_len, SaurcomAprsInfo* info_out, uint8_t* text, size_t cap);
size_t saurcom_ax25_aprs_type_name(int32_t id, uint8_t* out, size_t cap);
size_t saurcom_ax25_aprs_fmt_lat(double v, uint8_t* out, size_t cap);   // "25.2000N"
size_t saurcom_ax25_aprs_fmt_lon(double v, uint8_t* out, size_t cap);   // "55.3000E"

#ifdef __cplusplus
}
#endif
