// C ABI of the Rust crate rust/onair_dsn (Deep Space Network Now: the live dish, target and signal XML of eyes.nasa.gov and its
// configuration). Written by hand; the Rust test rust/onair/tests/headers.rs checks the functions against ffi.rs, and dect2/net/dsn.cpp
// checks the struct sizes against onair_dsn_feed_size() / onair_dsn_config_size().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ONAIR_DSN_MAX_STATIONS 4
#define ONAIR_DSN_MAX_DISHES 32
#define ONAIR_DSN_MAX_TARGETS 64
#define ONAIR_DSN_MAX_SIGNALS 128
#define ONAIR_DSN_MAX_SITES 4
#define ONAIR_DSN_MAX_SPACECRAFT 320

// Texts are NUL-terminated UTF-8. Unknown numbers are NaN.
typedef struct OnairDsnStation {
    char name[8];             // gdscc, mdscc, cdscc
    char friendly[24];        // Goldstone, Madrid, Canberra
    int64_t time_utc_ms;
    int64_t tz_offset_ms;     // add to time_utc_ms for the local time
} OnairDsnStation;

typedef struct OnairDsnDish {
    char name[8];             // DSS14 ...
    char activity[72];
    double azimuth_deg, elevation_deg;
    double wind_kmh;
    int32_t station;          // index into stations, -1 when none
    uint8_t mspa, array, ddor, _pad;
} OnairDsnDish;

typedef struct OnairDsnTarget {
    char name[16];            // call sign
    int32_t dish, id;
    double upleg_km, downleg_km, rtlt_s;
} OnairDsnTarget;

typedef struct OnairDsnSignal {
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
} OnairDsnSignal;

typedef struct OnairDsnFeed {
    int64_t timestamp_ms;
    uint32_t n_stations, n_dishes, n_targets, n_signals;
    OnairDsnStation stations[ONAIR_DSN_MAX_STATIONS];
    OnairDsnDish dishes[ONAIR_DSN_MAX_DISHES];
    OnairDsnTarget targets[ONAIR_DSN_MAX_TARGETS];
    OnairDsnSignal signals[ONAIR_DSN_MAX_SIGNALS];
} OnairDsnFeed;

typedef struct OnairDsnSite {
    char name[8];
    char friendly[24];
    double latitude, longitude;
} OnairDsnSite;

typedef struct OnairDsnDishInfo {
    char name[8];
    char kind[12];            // 70M, 34M, 34MHEF ...
    int32_t site;
} OnairDsnDishInfo;

typedef struct OnairDsnSpacecraft {
    char name[16];
    char friendly[64];
} OnairDsnSpacecraft;

typedef struct OnairDsnConfig {
    uint32_t n_sites, n_dishes, n_spacecraft, _pad;
    OnairDsnSite sites[ONAIR_DSN_MAX_SITES];
    OnairDsnDishInfo dishes[ONAIR_DSN_MAX_DISHES];
    OnairDsnSpacecraft spacecraft[ONAIR_DSN_MAX_SPACECRAFT];
} OnairDsnConfig;

// ABI version of this module (1).
uint32_t onair_dsn_version(void);
size_t onair_dsn_feed_size(void);
size_t onair_dsn_config_size(void);
// Reads a dsn.xml report into out (cleared first). False when the text is not a DSN report.
bool onair_dsn_parse(const uint8_t* xml, size_t n, OnairDsnFeed* out);
// Reads config.xml into out (cleared first). False when the text is not the DSN configuration.
bool onair_dsn_parse_config(const uint8_t* xml, size_t n, OnairDsnConfig* out);
// The full name of a call sign into out (NUL-terminated, cut to cap); its length, 0 when the config lacks it.
size_t onair_dsn_spacecraft_name(const OnairDsnConfig* cfg, const uint8_t* call, size_t call_len, uint8_t* out, size_t cap);
// The index of a dish in cfg->dishes, -1 when not listed.
int32_t onair_dsn_dish_index(const OnairDsnConfig* cfg, const uint8_t* dish, size_t dish_len);

#ifdef __cplusplus
}
#endif
