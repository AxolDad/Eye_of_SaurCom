// C ABI of the Rust crate rust/saurcom_acars (ACARS). Written by hand; the Rust test rust/saurcom/tests/headers.rs checks it against ffi.rs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAURCOM_ACARS_TEXT_CAP 220
#define SAURCOM_ACARS_DECODED_CAP 3328
// The fields of a parsed block. stage: 0 rejected (nothing set), 1 header only (the text does not start with STX), 2 acknowledgement without text,
// 3 complete. Like the C++ parser each stage keeps what the one before set.
typedef struct SaurcomAcarsBlock {
    uint8_t stage, final_block, mode, reg_len;
    uint8_t reg[7];   // the address without leading dots, control characters shown as dots
    uint8_t ack;      // '!' for NAK, '^' for ACK, else the block id acknowledged
    uint8_t label[2]; // the DEL of "_<DEL>" shows as 'd'
    uint8_t block_id, downlink, has_msg_num;
    uint8_t msg_num[3], msg_seq, flight_id[6];
    uint8_t sublabel_len, sublabel[2], mfi_len, mfi[2];
    uint16_t text_len, decoded_len;
    uint8_t text[SAURCOM_ACARS_TEXT_CAP];
    uint8_t decoded[SAURCOM_ACARS_DECODED_CAP];
} SaurcomAcarsBlock;

// ABI version of this module (1).
uint32_t saurcom_acars_version(void);
size_t saurcom_acars_block_size(void);   // sizeof(SaurcomAcarsBlock) as Rust sees it

// Block check sequence: CRC-16 KERMIT (reflected 0x1021, start 0, no final xor); 0 for a good block with its two check bytes (low byte first).
uint16_t saurcom_acars_crc_update(uint32_t crc, uint32_t byte);
uint16_t saurcom_acars_crc(const uint8_t* p, size_t n, uint32_t crc);
// The CRC residue left by one flipped bit; pos_from_end 0 is the last check byte, 1 the first. 0 when out of range.
uint16_t saurcom_acars_syndrome(int32_t pos_from_end, int32_t bit);
// Parity and check-sequence repair of txt (len bytes, in place); true when the block is good.
bool saurcom_acars_repair_block(uint8_t* txt, size_t len, uint32_t crc0, uint32_t crc1, int32_t max_parity_errors, int32_t* fixed_bits, int32_t* parity_errors);
// Name of a label as a static NUL-terminated string ("" when unknown).
const uint8_t* saurcom_acars_label_text(uint32_t a, uint32_t b);
// Short reading of fixed-format text (OOOI times, media advisory); length written, SIZE_MAX when cap is too small, 0 when nothing fits.
size_t saurcom_acars_decode_text(const uint8_t* label, size_t label_len, const uint8_t* text, size_t text_len, uint8_t* out, size_t cap);
// Parses a good block (bytes after SOH up to and including the suffix, parity removed); returns the stage reached.
int32_t saurcom_acars_parse_block(const uint8_t* t, size_t len, SaurcomAcarsBlock* m);

#ifdef __cplusplus
}
#endif
