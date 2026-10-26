/* Eye of ZorCom plugin ABI, version 1.
 *
 * A plugin is a shared library (.dylib / .so / .dll) plus a manifest.json. The library exports ONE function,
 *
 *     const ZorComPluginVTable* zorcom_plugin_entry(uint32_t host_abi_version);
 *
 * which returns a table of the plugin's functions. Everything crossing the boundary is plain C: fixed-width
 * integers, floats, NUL-terminated UTF-8 strings and pointers. No C++ types, no exceptions (catch everything
 * inside the plugin), no ownership transfer: memory passed to a call is only valid during that call, the
 * receiver copies what it needs.
 *
 * Every struct starts with struct_size so a newer host can talk to an older plugin and the other way round:
 * read only the fields that fit inside struct_size.
 *
 * Licence: this header is dedicated to the public domain (CC0-1.0) so that a plugin that only includes it and
 * uses the C ABI does not become a derivative work of Eye of ZorCom. See docs/plugins.md.
 */
#ifndef ZORCOM_PLUGIN_H
#define ZORCOM_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZORCOM_PLUGIN_ABI_VERSION 1u

#if defined(_WIN32)
#define ZORCOM_EXPORT __declspec(dllexport)
#else
#define ZORCOM_EXPORT __attribute__((visibility("default")))
#endif

/* Capability bits (ZorComPluginVTable.caps: what the plugin implements; ZorComHostApi.caps: what the host offers). */
#define ZORCOM_CAP_IQ_SINK 0x01u  /* plugin: process_iq() is implemented (a mode / source consumer) */
#define ZORCOM_CAP_EVENTS 0x02u   /* plugin: on_event() is implemented (an action plugin) */
#define ZORCOM_CAP_TICK 0x04u     /* plugin: tick() is implemented */
#define ZORCOM_CAP_SETTINGS 0x08u /* plugin: setting_changed() is implemented */

/* Log levels. */
#define ZORCOM_LOG_DEBUG 0
#define ZORCOM_LOG_INFO 1
#define ZORCOM_LOG_WARN 2
#define ZORCOM_LOG_ERROR 3

/* Lamps of a status line. */
#define ZORCOM_LAMP_OFF 0
#define ZORCOM_LAMP_OK 1
#define ZORCOM_LAMP_WARN 2
#define ZORCOM_LAMP_BAD 3

/* Column and cell types of a result table. */
#define ZORCOM_COL_TEXT 0
#define ZORCOM_COL_INT 1
#define ZORCOM_COL_FLOAT 2
#define ZORCOM_COL_FREQ_HZ 3
#define ZORCOM_COL_TIME 4 /* Unix seconds as a double */
#define ZORCOM_COL_BOOL 5

/* What a plotted float array means. */
#define ZORCOM_PLOT_SPECTRUM 0      /* power in dB, one value per bin; x0 / dx give the frequency axis */
#define ZORCOM_PLOT_SERIES 1        /* a time series, x0 / dx the time axis */
#define ZORCOM_PLOT_CONSTELLATION 2 /* interleaved I, Q pairs (n counts floats, so points = n / 2) */

/* Image formats. */
#define ZORCOM_IMG_RGBA 0 /* width * height * 4 bytes */
#define ZORCOM_IMG_PNG 1

typedef struct ZorComColumn {
  const char* name;
  int32_t type; /* ZORCOM_COL_* */
} ZorComColumn;

typedef struct ZorComCell {
  int32_t type; /* ZORCOM_COL_* */
  int64_t i;    /* INT, BOOL */
  double d;     /* FLOAT, FREQ_HZ, TIME */
  const char* s; /* TEXT (may be NULL = empty) */
} ZorComCell;

/* One named field of an automation event. */
typedef struct ZorComField {
  const char* key;
  int32_t is_text; /* 1: s is the value, 0: d is */
  double d;
  const char* s;
} ZorComField;

/* Metadata of one block of IQ samples. */
typedef struct ZorComIqMeta {
  uint32_t struct_size;
  double centre_hz;
  double sample_rate;
  double timestamp; /* Unix seconds of the first sample of the block */
  uint64_t first_sample_index; /* running count of samples since the stream started */
} ZorComIqMeta;

/* An automation event delivered to an action plugin. */
typedef struct ZorComEvent {
  uint32_t struct_size;
  const char* type; /* "adsb.aircraft" */
  const char* mode;
  double time;
  double freq_hz;
  uint32_t n_fields;
  const ZorComField* fields;
} ZorComEvent;

/* The callbacks the host gives the plugin. ctx is the host's own pointer: pass it back as the first argument,
 * never look inside. All callbacks may be called from the plugin's own worker thread only (the one that is
 * inside create / process_iq / on_event / tick); calling them from threads the plugin started itself is
 * allowed but they may drop data. Every call returns quickly and never blocks on the user interface.
 * Calls that return int return 0 on success and a negative value when refused (permission, limit, bad
 * argument): the plugin carries on. */
typedef struct ZorComHostApi {
  uint32_t struct_size;
  uint32_t abi_version; /* ZORCOM_PLUGIN_ABI_VERSION of the host */
  uint32_t caps;        /* reserved, 0 */
  void* ctx;

  /* diagnostics (shown in the plugin manager's log) */
  void (*log)(void* ctx, int32_t level, const char* msg);

  /* settings declared in the manifest, as text / number / flag (the default when unset) */
  int32_t (*get_setting_text)(void* ctx, const char* key, char* out, uint32_t cap);
  double (*get_setting_number)(void* ctx, const char* key, double def);
  int32_t (*get_setting_flag)(void* ctx, const char* key, int32_t def);

  /* IQ: ask for a stream at this centre frequency (0 = keep the user's) and rate (0 = keep). The host may give
   * something else; process_iq() carries the real values in ZorComIqMeta. */
  int32_t (*request_iq)(void* ctx, double centre_hz, double sample_rate);

  /* results shown by the generic Plugin panel */
  int32_t (*table_define)(void* ctx, const char* table_id, const char* title, uint32_t n_cols, const ZorComColumn* cols);
  int32_t (*table_row)(void* ctx, const char* table_id, uint32_t n_cells, const ZorComCell* cells);
  int32_t (*table_clear)(void* ctx, const char* table_id);
  int32_t (*status)(void* ctx, const char* key, const char* text, int32_t lamp);
  int32_t (*result_log)(void* ctx, const char* line);
  int32_t (*plot)(void* ctx, const char* plot_id, const char* title, int32_t kind, const float* data, uint32_t n, double x0, double dx);
  int32_t (*image)(void* ctx, const char* image_id, int32_t format, uint32_t width, uint32_t height, const uint8_t* data, uint32_t size);
  int32_t (*map_point)(void* ctx, const char* point_id, const char* label, double lat, double lon);

  /* automation: becomes the event "plugin.<type>" for the rules engine (rules see the fields) */
  int32_t (*emit_event)(void* ctx, const char* type, uint32_t n_fields, const ZorComField* fields);

  /* permission "audio-out": mono float PCM in -1..1 */
  int32_t (*audio_out)(void* ctx, const float* pcm, uint32_t n, double sample_rate);

  /* permission "filesystem": the plugin's own data folder (UTF-8 path). Refused without the permission. */
  int32_t (*data_dir)(void* ctx, char* out, uint32_t cap);
} ZorComHostApi;

/* The plugin's functions. Optional entries are NULL. instance = what create() returned. */
typedef struct ZorComPluginVTable {
  uint32_t struct_size;
  uint32_t abi_version; /* ZORCOM_PLUGIN_ABI_VERSION the plugin was built against */
  uint32_t caps;        /* ZORCOM_CAP_* */
  const char* id;       /* must equal manifest.json "id" */

  /* One instance per running mode (mode_id = the manifest's mode id, "" for an action plugin). Return NULL to refuse. */
  void* (*create)(const ZorComHostApi* host, const char* mode_id);
  void (*destroy)(void* instance);
  /* The stream parameters changed (also called once before the first block). */
  void (*configure)(void* instance, double centre_hz, double sample_rate);
  /* n_complex samples of interleaved float I, Q. Return fast: a call that takes longer than the host's
   * watchdog limit (2 s) gets the plugin switched off. */
  void (*process_iq)(void* instance, const float* iq, uint32_t n_complex, const ZorComIqMeta* meta);
  void (*on_event)(void* instance, const ZorComEvent* ev);
  void (*tick)(void* instance, double now_unix);
  void (*setting_changed)(void* instance, const char* key);
} ZorComPluginVTable;

typedef const ZorComPluginVTable* (*zorcom_plugin_entry_fn)(uint32_t host_abi_version);

ZORCOM_EXPORT const ZorComPluginVTable* zorcom_plugin_entry(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif
#endif
