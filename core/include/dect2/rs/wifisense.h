// C ABI of the Rust crate rust/onair_wifisense (WiFi sensing from the CSI frames of RuView's ESP32 nodes: presence, motion,
// breathing and heart rate). Written by hand; the Rust test rust/onair/tests/headers.rs checks it against ffi.rs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OnairWifiSenseSensor OnairWifiSenseSensor;

// One node's state after a frame. Rates are 0 and their confidence 0 while unknown; confidence runs 0 - 1.
typedef struct OnairWifiSense {
    uint8_t node_id;
    uint8_t presence;   // 1 when someone is in the room
    uint8_t n_antennas;
    uint8_t _pad;
    uint32_t n_subcarriers;
    uint32_t freq_mhz;
    uint32_t seq;
    int32_t rssi, noise;   // dBm
    double motion;         // mean channel disturbance relative to the mean amplitude, smoothed over about a second
    double breathing_bpm, breathing_conf;
    double heart_bpm, heart_conf;
    uint64_t frames;
} OnairWifiSense;

// ABI version of this module (1).
uint32_t onair_wifisense_version(void);
size_t onair_wifisense_struct_size(void);
// The node id of a CSI datagram, or -1 when it is not a whole ADR-018 CSI frame.
int32_t onair_wifisense_node(const uint8_t* d, size_t n);
// A sensor for one node sending frames_per_sec frames (RuView firmware: 50); free it with onair_wifisense_free.
OnairWifiSenseSensor* onair_wifisense_new(double frames_per_sec);
void onair_wifisense_free(OnairWifiSenseSensor* s);
// Feeds one datagram to the node's sensor; false (out untouched) when it is not a CSI frame.
bool onair_wifisense_feed(OnairWifiSenseSensor* s, const uint8_t* d, size_t n, OnairWifiSense* out);

#ifdef __cplusplus
}
#endif
