// C ABI of the Rust crate rust/zorcom_hfdl (HFDL). Written by hand; the Rust test rust/zorcom/tests/headers.rs checks it against ffi.rs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZorcomHfdlS8 { uint16_t len; uint8_t b[8]; } ZorcomHfdlS8;
typedef struct ZorcomHfdlS24 { uint16_t len; uint8_t b[24]; } ZorcomHfdlS24;
typedef struct ZorcomHfdlS32 { uint16_t len; uint8_t b[32]; } ZorcomHfdlS32;
typedef struct ZorcomHfdlS256 { uint16_t len; uint8_t b[256]; } ZorcomHfdlS256;
typedef struct ZorcomHfdlS3072 { uint16_t len; uint8_t b[3072]; } ZorcomHfdlS3072;

// One of the eight rates (the M1 shift index).
typedef struct ZorcomHfdlRate { int32_t m1, bps, bits_per_sym; uint8_t quarter, two_slots; int32_t blocks, col_shift; } ZorcomHfdlRate;

// One ground station; name is static, NUL terminated.
typedef struct ZorcomHfdlStation { int32_t id, nkhz; double lat, lon; int32_t khz[8]; const char* name; } ZorcomHfdlStation;

// One decoded SPDU or LPDU (see hfdl_proto.cpp). Defaults when a PDU sets nothing: ac_id -1, frame_index -1, utc_sec -1, acars_at -1, text empty. acars_at is the
// offset in the PDU of the octets after the HFNPDU's FF FF (the ACARS block, maybe starting with SOH), acars_len the octets from there to the end of the LPDU
// body; the caller decodes them with the ACARS code and appends to the detail text.
typedef struct ZorcomHfdlMsg {
    uint8_t squitter, uplink, utc_sync, acars, has_pos;
    int32_t gs_id, ac_id, frame_index, utc_sec, acars_at, acars_len, active_n;
    uint32_t icao;
    double lat, lon;
    int32_t active_khz[20];
    ZorcomHfdlS32 kind;
    ZorcomHfdlS24 hfnpdu;
    ZorcomHfdlS8 flight;
    ZorcomHfdlS24 pos_kind;
    ZorcomHfdlS256 summary;
    ZorcomHfdlS3072 detail;
} ZorcomHfdlMsg;

// ABI version of this module (2).
uint32_t zorcom_hfdl_version(void);
// sizeof of ZorcomHfdlRate, ZorcomHfdlMsg and ZorcomHfdlStation in this order (out has room for 3): checked at start-up.
void zorcom_hfdl_struct_sizes(size_t* out);

// Rates and burst sizes (m1 clamped into 0..7).
void zorcom_hfdl_rate(int32_t m1, ZorcomHfdlRate* out);
int32_t zorcom_hfdl_burst_syms(int32_t m1);
int32_t zorcom_hfdl_coded_bits(int32_t m1);
int32_t zorcom_hfdl_payload_octets(int32_t m1);

// Known symbols: A and M sequence bits (k 0..126, shift 0..7); the 531 symbols from A1 to the last training block (+-1) into out (cap >= 531, false when smaller).
int32_t zorcom_hfdl_a_bit(int32_t k);
int32_t zorcom_hfdl_m_bit(int32_t shift, int32_t k);
bool zorcom_hfdl_preamble_ref(int32_t m1, int8_t* out, size_t cap);

// Scrambler signs (+1 / -1) for n data symbols (restarts every 120).
void zorcom_hfdl_scrambler_signs(int8_t* out, size_t n);
// Interleaver of rate m1 into out (cap ints: twice the coded bits, the second half is scratch). Returns the coded bit count (permutation in out[0..count]) or
// SIZE_MAX when cap is too small.
size_t zorcom_hfdl_interleave(int32_t m1, int32_t* out, size_t cap);

// Convolutional code (rate 1/2, K = 7, 0x6D output first then 0x4F): n bits (one per byte) -> 2n coded bits. Soft Viterbi: soft_len >= 2n values (positive = 0), n bits
// out; surv is scratch of n uint64. False when a buffer is too small.
bool zorcom_hfdl_conv_encode(const uint8_t* bits, size_t n, uint8_t* out, size_t cap);
bool zorcom_hfdl_viterbi(const float* soft, size_t soft_len, size_t n, uint8_t* out, size_t out_cap, uint64_t* surv, size_t surv_cap);

// FCS (CRC-16/X.25, sent low octet first) of n octets; check of n octets followed by their FCS (n + 2 octets are read).
uint16_t zorcom_hfdl_fcs(const uint8_t* p, size_t n);
bool zorcom_hfdl_fcs_ok(const uint8_t* p, size_t n);

// Ground station table, frequency masks (out[20]; returns the count; a bit beyond the table gives -1 - bit), station that lists a frequency (0 none).
size_t zorcom_hfdl_station_count(void);
bool zorcom_hfdl_station_get(size_t i, ZorcomHfdlStation* out);
size_t zorcom_hfdl_mask_khz(int32_t gs_id, uint32_t mask, int32_t* out);
int32_t zorcom_hfdl_station_on_khz(int32_t khz);

// Parses the PDU of one burst into up to cap messages (128 cover any PDU); *count receives how many. False when no check sequence comes out right.
bool zorcom_hfdl_parse_pdu(const uint8_t* p, size_t n, ZorcomHfdlMsg* out, size_t cap, size_t* count);

#ifdef __cplusplus
}
#endif
