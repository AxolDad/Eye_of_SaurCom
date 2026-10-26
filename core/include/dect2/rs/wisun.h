// C ABI of the Rust crate rust/zorcom_wisun (Wi-SUN FAN / IEEE 802.15.4g SUN FSK frame layers). Written by hand; the Rust test rust/zorcom/tests/headers.rs checks it
// against ffi.rs. Flags are 0 / 1 bytes; text is a length plus bytes (no NUL).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZorcomWisunS64 { uint16_t len; uint8_t b[64]; } ZorcomWisunS64;
typedef struct ZorcomWisunS256 { uint16_t len; uint8_t b[256]; } ZorcomWisunS256;
typedef struct ZorcomWisunS8192 { uint16_t len; uint8_t b[8192]; } ZorcomWisunS8192;

// The 2-octet PHR: mode switch, FCS type (1: 2-octet FCS), data whitening, 11 bit PSDU length (FCS included).
typedef struct ZorcomWisunPhr { uint8_t mode_switch, fcs2, whitened; int32_t length; } ZorcomWisunPhr;
// A unicast (US-IE) or broadcast (BS-IE) schedule; -1 / 0 when a field is not in the IE.
typedef struct ZorcomWisunSchedule {
    uint8_t present;
    uint32_t bcast_interval_ms;
    int32_t bsi, dwell_ms, clock_drift, timing_accuracy, ch_plan, ch_function, excluded, reg_domain;
    int32_t op_class, ch_plan_id;
    double ch0_mhz;
    int32_t spacing_code, num_channels, fixed_channel, excluded_count;
} ZorcomWisunSchedule;
// A parsed MPDU (FCS excluded): the fields of wisun::Frame.
typedef struct ZorcomWisunFrame {
    uint8_t ok, secured, pending, ack_req, pan_compress, seq_suppressed, ie_list, has_dest_pan;
    uint8_t has_src_pan, has_bt, has_fc, has_lbc, has_pan_ie, use_parent_bs, rpl_routing, has_net_name;
    uint8_t has_gtk_hash, encrypted;
    int32_t kind, version, dest_mode, src_mode, seq;
    uint16_t dest_pan, src_pan, dest_short, src_short;
    uint64_t dest_ext, src_ext;
    int32_t sec_level, key_id_mode, key_index;
    uint32_t frame_counter;
    int32_t utt;
    uint32_t ufsi;
    int32_t bsn;
    uint32_t bio;
    int32_t rsl_dbm, tx_flow, rx_flow;
    uint64_t ea_eui;
    uint32_t lfn_bcast_ms;
    ZorcomWisunSchedule us, bs;
    int32_t pan_size, routing_cost, tps_version, pan_version, lfn_version, phy_mode_count;
    int32_t phy_modes[2048];
    int32_t mpx_id, kmp_id, eapol_type, eap_code, eap_type;
    uint64_t payload_at, payload_len;
    ZorcomWisunS64 net_name;
    ZorcomWisunS8192 ies;
    ZorcomWisunS256 info;
} ZorcomWisunFrame;

// ABI version of this module (2).
uint32_t zorcom_wisun_version(void);
// sizeof of ZorcomWisunFrame, ZorcomWisunSchedule and ZorcomWisunPhr in this order (out has room for 3): checked at start-up.
void zorcom_wisun_struct_sizes(size_t* out);

// PHY: PHR split / build, PN9 whitening in place (own inverse), the 2- and 4-octet FCS, FCS check of a PSDU with the FCS at its end.
void zorcom_wisun_parse_phr(uint32_t v, ZorcomWisunPhr* out);
uint16_t zorcom_wisun_build_phr(const ZorcomWisunPhr* p);
void zorcom_wisun_whiten(uint8_t* d, size_t n);
uint16_t zorcom_wisun_crc16(const uint8_t* d, size_t n);   // CRC-16/KERMIT
uint32_t zorcom_wisun_crc32(const uint8_t* d, size_t n);   // IEEE 802.3 CRC-32
bool zorcom_wisun_fcs_ok(const uint8_t* p, size_t n, uint32_t fcs2);

// MAC: parse an MPDU of n octets (FCS excluded); the frame says how far the header could be read (ok).
void zorcom_wisun_parse(const uint8_t* d, size_t n, ZorcomWisunFrame* out);

// Names (static NUL terminated text, "?" for unknown values) and the schedule text (length, 0 when not present, SIZE_MAX when cap is too small).
const char* zorcom_wisun_utt_name(int32_t t);
const char* zorcom_wisun_utt_short(int32_t t);
const char* zorcom_wisun_ch_function_name(int32_t f);
const char* zorcom_wisun_reg_domain_name(int32_t c);
size_t zorcom_wisun_schedule_text(const ZorcomWisunSchedule* s, uint8_t* out, size_t cap);

#ifdef __cplusplus
}
#endif
