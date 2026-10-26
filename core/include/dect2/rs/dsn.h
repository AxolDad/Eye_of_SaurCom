// C ABI of the Rust crate rust/saurcom_dsn (Deep Space Network Now: the live dish, target and signal XML of eyes.nasa.gov and its
// configuration). Written by hand; the Rust test rust/saurcom/tests/headers.rs checks the functions against ffi.rs, and dect2/net/dsn.cpp
// checks the struct sizes against saurcom_dsn_feed_size() / saurcom_dsn_config_size().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SAURCOM_DSN_MAX_STATIONS 4
#define SAURCOM_DSN_MAX_DISHES 32
#define SAURCOM_DSN_MAX_TARGETS 64
#define SAURCOM_DSN_MAX_SIGNALS 128
#define SAURCOM_DSN_MAX_SITES 4
#define SAURCOM_DSN_MAX_SPACECRAFT 320

// Texts are NUL-terminated UTF-8. Unknown numbers are NaN.
typedef struct SaurcomDsnStation {
    char name[8];             // gdscc, mdscc, cdscc
    char friendly[24];        // Goldstone, Madrid, Canberra
    int64_t time_utc_ms;
    int64_t tz_offset_ms;     // add to time_utc_ms for the local time
} SaurcomDsnStation;

typedef struct SaurcomDsnDish {
    char name[8];             // DSS14 ...
    char activity[72];
    double azimuth_deg, elevation_deg;
    double wind_kmh;
    int32_t station;          // index into stations, -1 when none
    uint8_t mspa, array, ddor, _pad;
} SaurcomDsnDish;

typedef struct SaurcomDsnTarget {
    char name[16];            // call sign
    int32_t dish, id;
    double upleg_km, downleg_km, rtlt_s;
} SaurcomDsnTarget;

typedef struct SaurcomDsnSignal {
    char spacecraft[16];
    int32_t dish, spacecraft_id;
    uint8_t up;               // 1 uplink, 0 downlink
    uint8_t active;
    uint8_t kind;             // 0 none, 1 carrier, 2 data, 3 other
    uint8_t band;             // 'S', 'X', 'K' (Ka), 0 when not given
    uint8_t power_flagged;    // downlink power had the -300 dB flag (corrected)
    uint8_t _pad[3];
    double data_rate_bps;
    double frequency_hz;      // 0 when not published
    double power;             // uplink kW, downlink dBm
} SaurcomDsnSignal;

typedef struct SaurcomDsnFeed {
    int64_t timestamp_ms;
    uint32_t n_stations, n_dishes, n_targets, n_signals;
    SaurcomDsnStation stations[SAURCOM_DSN_MAX_STATIONS];
    SaurcomDsnDish dishes[SAURCOM_DSN_MAX_DISHES];
    SaurcomDsnTarget targets[SAURCOM_DSN_MAX_TARGETS];
    SaurcomDsnSignal signals[SAURCOM_DSN_MAX_SIGNALS];
} SaurcomDsnFeed;

typedef struct SaurcomDsnSite {
    char name[8];
    char friendly[24];
    double latitude, longitude;
} SaurcomDsnSite;

typedef struct SaurcomDsnDishInfo {
    char name[8];
    char kind[12];            // 70M, 34M, 34MHEF ...
    int32_t site;
} SaurcomDsnDishInfo;

typedef struct SaurcomDsnSpacecraft {
    char name[16];
    char friendly[64];
} SaurcomDsnSpacecraft;

typedef struct SaurcomDsnConfig {
    uint32_t n_sites, n_dishes, n_spacecraft, _pad;
    SaurcomDsnSite sites[SAURCOM_DSN_MAX_SITES];
    SaurcomDsnDishInfo dishes[SAURCOM_DSN_MAX_DISHES];
    SaurcomDsnSpacecraft spacecraft[SAURCOM_DSN_MAX_SPACECRAFT];
} SaurcomDsnConfig;

// ABI version of this module (1).
uint32_t saurcom_dsn_version(void);
size_t saurcom_dsn_feed_size(void);
size_t saurcom_dsn_config_size(void);
// Reads a dsn.xml report into out (cleared first). False when the text is not a DSN report.
bool saurcom_dsn_parse(const uint8_t* xml, size_t n, SaurcomDsnFeed* out);
// Reads config.xml into out (cleared first). False when the text is not the DSN configuration.
bool saurcom_dsn_parse_config(const uint8_t* xml, size_t n, SaurcomDsnConfig* out);
// The full name of a call sign into out (NUL-terminated, cut to cap); its length, 0 when the config lacks it.
size_t saurcom_dsn_spacecraft_name(const SaurcomDsnConfig* cfg, const uint8_t* call, size_t call_len, uint8_t* out, size_t cap);
// The index of a dish in cfg->dishes, -1 when not listed.
int32_t saurcom_dsn_dish_index(const SaurcomDsnConfig* cfg, const uint8_t* dish, size_t dish_len);

#ifdef __cplusplus
}
#endif
