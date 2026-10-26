// C ABI of the Rust crate rust/zorcom_dmr (DMR frame layer). Written by hand; the Rust test rust/zorcom/tests/headers.rs checks it against ffi.rs.
// Bits are one per byte, first bit on the air first; "bitsN" arrays are exactly N bytes.
// Text functions return the length written, or SIZE_MAX when the buffer is too small. Names are static NUL-terminated strings.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZorcomDmrFullLc { uint8_t pf; int32_t flco, fid; uint8_t svc; uint32_t dst, src; uint8_t raw[9]; } ZorcomDmrFullLc;
typedef struct ZorcomDmrCsbk { uint8_t lb, pf; int32_t opcode, fid; uint8_t data[8]; } ZorcomDmrCsbk;
typedef struct ZorcomDmrDataHeader {
    int32_t dpf, sap;
    uint8_t group, resp;
    uint32_t dst, src;
    int32_t blocks, pad;
    uint8_t fmf;
    int32_t fsn, ns, dd, sp, dp;
    uint8_t resync, sarq;
    int32_t status;
} ZorcomDmrDataHeader;
typedef struct ZorcomDmrDataBlock { uint8_t bytes[24]; uint32_t len, dbsn; uint8_t crc_ok; } ZorcomDmrDataBlock;
typedef struct ZorcomDmrTa { int32_t format, length; uint8_t have; uint8_t data[28]; } ZorcomDmrTa;

// ABI version of this module (2).
uint32_t zorcom_dmr_version(void);
// sizeof of the five structs above in this order (full LC, CSBK, data header, data block, talker alias): the C++ side checks them at start-up.
void zorcom_dmr_struct_sizes(size_t out[5]);

// ---- short codes (annex B.3). Decoders return the error count (or -1) and write the information word only on success.
uint32_t zorcom_dmr_golay2008_parity(uint32_t info8);
int32_t zorcom_dmr_golay2008_decode(uint32_t word20, uint32_t* info8);
uint32_t zorcom_dmr_qr1676_parity(uint32_t info7);
int32_t zorcom_dmr_qr1676_decode(uint32_t word16, uint32_t* info7);
uint32_t zorcom_dmr_hamming743_parity(uint32_t info4);
int32_t zorcom_dmr_hamming743_decode(uint32_t word7, uint32_t* info4);
// Product code Hamming kinds: 0 (13,9,3), 1 (15,11,3), 2 (16,11,4), 3 (17,12,3). Correct: 0 clean, 1 one bit corrected, -1 not correctable.
uint32_t zorcom_dmr_hamming_parity(int32_t kind, uint32_t info);
int32_t zorcom_dmr_hamming_correct(int32_t kind, uint32_t* word);
int32_t zorcom_dmr_hamming_info_bits(int32_t kind);
int32_t zorcom_dmr_hamming_parity_bits(int32_t kind);
// Reed-Solomon (12,9): parity of msg[9] into parity[3]; correct word[12] (0 clean, 1 corrected, -1 failed).
void zorcom_dmr_rs129_parity(const uint8_t* msg, uint8_t* parity);
int32_t zorcom_dmr_rs129_correct(uint8_t* word);
uint16_t zorcom_dmr_crc_ccitt(const uint8_t* d, size_t n);
uint8_t zorcom_dmr_crc8_short(uint32_t bits28);
uint16_t zorcom_dmr_crc9(const uint8_t* d, size_t n, uint32_t dbsn);
uint32_t zorcom_dmr_crc32_msg(const uint8_t* d, size_t n);
uint8_t zorcom_dmr_checksum5(const uint8_t* lc9);

// ---- block product codes, CACH, trellis, rate 1
void zorcom_dmr_bptc196_encode(const uint8_t* info96, uint8_t* tx196);
int32_t zorcom_dmr_bptc196_decode(const uint8_t* tx196, uint8_t* info96);          // bits corrected, -1 (info96 untouched)
void zorcom_dmr_emb_lc_encode(const uint8_t* lc9, uint8_t* frag128);              // four fragments of 32 bits, one after the other
int32_t zorcom_dmr_emb_lc_decode(const uint8_t* frag128, uint8_t* lc9);           // -1 uncorrectable (lc untouched), -2 checksum wrong
void zorcom_dmr_short_lc_encode(uint32_t lc28, uint8_t* piece68);                 // four pieces of 17 bits
int32_t zorcom_dmr_short_lc_decode(const uint8_t* piece68, uint32_t* lc28);       // -1 uncorrectable, -2 CRC mismatch (lc28 written)
void zorcom_dmr_cach_encode(int32_t at, int32_t tc, int32_t lcss, uint32_t payload17, uint8_t* out24);
int32_t zorcom_dmr_cach_decode(const uint8_t* in24, int32_t* at, int32_t* tc, int32_t* lcss, uint32_t* payload17);   // payload always set
void zorcom_dmr_trellis34_encode(const uint8_t* data18, uint8_t* tx196);
int32_t zorcom_dmr_trellis34_decode(const float* symbols98, uint8_t* data18);     // dibits differing from the best path, -1 no path
void zorcom_dmr_rate1_encode(const uint8_t* data24, uint8_t* tx196);
bool zorcom_dmr_rate1_decode(const uint8_t* tx196, uint8_t* data24);              // true: pad bits are zero

// ---- sync patterns and bursts
const uint8_t* zorcom_dmr_sync_name(int32_t s);
uint64_t zorcom_dmr_sync_word(int32_t s);
int32_t zorcom_dmr_sync_direct_slot(int32_t s);
int32_t zorcom_dmr_sync_of(int32_t family, uint32_t voice);
bool zorcom_dmr_sync_symbols(int32_t s, int8_t* out24);
int32_t zorcom_dmr_match_sync(const uint8_t* centre48, int32_t max_diff, int32_t* diff);
const uint8_t* zorcom_dmr_data_type_name(int32_t dt);
const uint8_t* zorcom_dmr_data_type_short(int32_t dt);
bool zorcom_dmr_make_data_burst(int32_t cc, int32_t dt, const uint8_t* payload196, int32_t sync, uint8_t* out264);
bool zorcom_dmr_make_voice_burst(const uint8_t* vs216, int32_t sync, int32_t cc, int32_t pi, int32_t lcss, const uint8_t* emb32, uint8_t* out264);
bool zorcom_dmr_burst_slot_type(const uint8_t* burst, size_t n, int32_t* cc, int32_t* dt, int32_t* errors);
bool zorcom_dmr_burst_emb(const uint8_t* burst, size_t n, uint8_t* emb32, int32_t* cc, int32_t* pi, int32_t* lcss, int32_t* errors);   // emb32 always set
size_t zorcom_dmr_bits_to_symbols(const uint8_t* bits, size_t n, int8_t* out, size_t cap);

// ---- link control, CSBK, CRC blocks
void zorcom_dmr_pack_full_lc(const ZorcomDmrFullLc* lc, uint8_t* out9);
void zorcom_dmr_parse_full_lc(const uint8_t* in9, ZorcomDmrFullLc* lc);
const uint8_t* zorcom_dmr_flco_name(int32_t f);
size_t zorcom_dmr_service_options_text(uint32_t svc, uint8_t* out, size_t cap);
void zorcom_dmr_lc_burst_info(const uint8_t* lc9, uint32_t terminator, uint8_t* info96);
int32_t zorcom_dmr_lc_burst_decode(const uint8_t* info96, uint32_t terminator, uint8_t* lc9);   // 0 clean, 1 one octet corrected, -1 failed
void zorcom_dmr_crc_block_info(const uint8_t* data10, uint32_t mask, uint8_t* info96);
bool zorcom_dmr_crc_block_check(const uint8_t* info96, uint32_t mask, uint8_t* data10);     // data10 always written
void zorcom_dmr_csbk_info(const ZorcomDmrCsbk* c, uint8_t* info96);
bool zorcom_dmr_csbk_parse(const uint8_t* info96, ZorcomDmrCsbk* c);                         // c written only when true
const uint8_t* zorcom_dmr_csbk_name(int32_t op, int32_t fid);

// ---- data packets
const uint8_t* zorcom_dmr_dpf_name(int32_t d);
const uint8_t* zorcom_dmr_sap_name(int32_t s);
void zorcom_dmr_pack_data_header(const ZorcomDmrDataHeader* h, uint8_t* out10);
bool zorcom_dmr_parse_data_header(const uint8_t* in10, ZorcomDmrDataHeader* h);              // h always written
int32_t zorcom_dmr_data_block_bytes(int32_t dt, uint32_t confirmed);
void zorcom_dmr_data_block_encode(int32_t dt, const uint8_t* user, size_t n, uint32_t confirmed, uint32_t dbsn, uint8_t* out196);
bool zorcom_dmr_data_block_parse(int32_t dt, const uint8_t* oct, size_t n, uint32_t confirmed, ZorcomDmrDataBlock* out);

// ---- text and talker alias
const uint8_t* zorcom_dmr_dd_name(int32_t dd);
size_t zorcom_dmr_text_from_bytes(const uint8_t* p, size_t n, int32_t format, uint32_t ta, uint8_t* out, size_t cap);   // at most 2n + 80 bytes
void zorcom_dmr_ta_add(ZorcomDmrTa* t, const ZorcomDmrFullLc* lc);
bool zorcom_dmr_ta_complete(const ZorcomDmrTa* t);
size_t zorcom_dmr_ta_text(const ZorcomDmrTa* t, uint8_t* out, size_t cap);
int32_t zorcom_dmr_alias_pieces(int32_t format, int32_t length);

#ifdef __cplusplus
}
#endif
