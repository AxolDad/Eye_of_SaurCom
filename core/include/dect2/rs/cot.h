// C ABI of the Rust crate rust/onair_cot (Cursor on Target for ATAK, WinTAK, iTAK and TAK servers). Written by hand; the Rust test
// rust/onair/tests/headers.rs checks it against ffi.rs. Text goes in as (pointer, length) UTF-8; strings handed out are static and NUL-terminated.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A received contact. hae is NaN when unknown; times are ms since 1970 UTC (0 when the event had none).
typedef struct OnairCotEvent {
    uint8_t uid[96];
    uint8_t cot_type[48];
    uint8_t callsign[64];
    double lat, lon, hae;
    int64_t time_ms, stale_ms;
} OnairCotEvent;

// ABI version of this module (1).
uint32_t onair_cot_version(void);
size_t onair_cot_struct_size(void);
// The CoT type of a contact kind: 0 aircraft, 1 vessel, 2 station, 3 balloon, 4 mesh node ("" for another value).
const uint8_t* onair_cot_type(uint32_t kind);
// One event's XML into out; the length written, or SIZE_MAX when out is too small, a coordinate is out of range or uid is empty.
// Unknown values: hae NaN, course_deg or speed_mps negative. stale_sec: how long the contact stays on the other end's map.
size_t onair_cot_encode(const uint8_t* uid, size_t uid_len, const uint8_t* cot_type, size_t type_len,
                        const uint8_t* callsign, size_t callsign_len, const uint8_t* remarks, size_t remarks_len,
                        double lat, double lon, double hae, double course_deg, double speed_mps,
                        int64_t time_ms, int32_t stale_sec, uint8_t* out, size_t cap);
// Reads one received event document; false (out untouched) unless it has a uid, a type and a valid point.
bool onair_cot_decode(const uint8_t* xml, size_t n, OnairCotEvent* out);

#ifdef __cplusplus
}
#endif
