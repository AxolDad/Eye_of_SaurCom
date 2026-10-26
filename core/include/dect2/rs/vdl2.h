// C ABI of the Rust crate rust/saurcom_vdl2 (VDL Mode 2 link layers). Written by hand; the Rust test rust/saurcom/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte; flags are 0 / 1 bytes; text is a length plus bytes (no NUL).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SaurcomVdl2S16 { uint16_t len; uint8_t b[16]; } SaurcomVdl2S16;
typedef struct SaurcomVdl2S64 { uint16_t len; uint8_t b[64]; } SaurcomVdl2S64;
typedef struct SaurcomVdl2S512 { uint16_t len; uint8_t b[512]; } SaurcomVdl2S512;
typedef struct SaurcomVdl2S16384 { uint16_t len; uint8_t b[16384]; } SaurcomVdl2S16384;

// How a burst's data and FEC octets are laid out (Vdl2Layout).
typedef struct SaurcomVdl2Layout { int32_t data_oct, fec_oct, blocks, last_len, last_fec; } SaurcomVdl2Layout;
// An AVLC address: 24 bit address, 3 bit type, A/G or C/R status bit (Vdl2Addr).
typedef struct SaurcomVdl2Addr { uint32_t addr; int32_t type; uint8_t status; } SaurcomVdl2Addr;
// The decoded part of an AVLC frame (see vdl2_proto.cpp). ns / nr are -1 when the frame has none; acars_at is the offset of the SOH of an ACARS block the caller
// decodes (-1 none). Text fields are empty when the frame sets nothing there.
typedef struct SaurcomVdl2Parsed {
    uint8_t ok, from_air, from_ground, on_ground, response, pf, has_pos, has_alt, frame_kind;
    int32_t dst_type, src_type, ns, nr, alt_ft, acars_at;
    uint32_t dst, src;
    double lat, lon;
    SaurcomVdl2S16 frame_name, proto;
    SaurcomVdl2S512 summary;
    SaurcomVdl2S64 pos_kind;
    SaurcomVdl2S16384 detail;
} SaurcomVdl2Parsed;

// ABI version of this module (2).
uint32_t saurcom_vdl2_version(void);
// sizeof of SaurcomVdl2Parsed, SaurcomVdl2Layout and SaurcomVdl2Addr in this order (out has room for 3): checked at start-up.
void saurcom_vdl2_struct_sizes(size_t* out);

// Scrambler (one pass scrambles and descrambles n bits in place), header code, layout.
void saurcom_vdl2_scramble(uint8_t* bits, size_t n);
uint32_t saurcom_vdl2_header_word(uint32_t len_bits);
bool saurcom_vdl2_header_decode(uint32_t word, uint32_t* len, bool* corrected);   // *corrected always written, *len when computed
int32_t saurcom_vdl2_last_fec(int32_t n);
bool saurcom_vdl2_layout(uint32_t len_bits, SaurcomVdl2Layout* out);                // always written; false when it cannot be a burst

// Reed-Solomon (255,249): parity of 249 octets (6 out); correct a 255 octet codeword in place with n_parity (0..6) parity octets present: octets corrected or -1.
void saurcom_vdl2_rs_encode(const uint8_t* data249, uint8_t* parity6);
int32_t saurcom_vdl2_rs_decode(uint8_t* cw255, int32_t n_parity);

// Burst body: gather bits[0..n] into blocks with layout l, correct them, write the first len_bits data bits to out (capacity cap); returns the bits written or -1
// (a block cannot be corrected); *rs_fixed receives the octets corrected so far.
int64_t saurcom_vdl2_burst_data(const uint8_t* bits, size_t n, const SaurcomVdl2Layout* l, uint32_t len_bits, uint8_t* out, size_t cap, int32_t* rs_fixed);

// HDLC: FCS to send, FCS check of a frame with its FCS, deframer: frames from n bits into out (octets one after the other) and lens; false when a buffer is too small
// (n / 8 + 1 octets and n / 16 + 1 frames suffice).
uint16_t saurcom_vdl2_fcs(const uint8_t* p, size_t n);
bool saurcom_vdl2_fcs_ok(const uint8_t* p, size_t n);
bool saurcom_vdl2_hdlc_frames(const uint8_t* bits, size_t n, uint8_t* out, size_t out_cap, uint32_t* lens, size_t lens_cap, size_t* frames, size_t* octets);

// AVLC: addresses, type names (static NUL terminated text), frame decode (false when n < 9; n octets without the FCS).
void saurcom_vdl2_put_addr(const SaurcomVdl2Addr* a, uint32_t last, uint8_t* out4);
void saurcom_vdl2_get_addr(const uint8_t* in4, SaurcomVdl2Addr* out);
const char* saurcom_vdl2_addr_type_name(int32_t t);
bool saurcom_vdl2_parse_frame(const uint8_t* f, size_t n, SaurcomVdl2Parsed* out);

#ifdef __cplusplus
}
#endif
