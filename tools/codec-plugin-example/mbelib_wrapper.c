/* Reference wrapper: an Eye of ZorCom speech codec plug-in around YOUR OWN mbelib. This file contains no codec; it only calls the library you build.
 * Eye of ZorCom does not ship mbelib or any binary built from this file (see docs/CODECS.md, "AMBE+2, AMBE and IMBE plug-ins").
 *
 * Choose the codec at compile time:  -DCODEC_AMBE2 (DMR, NXDN, dPMR; plug-in file zorcom-codec-ambe2)
 *                                    -DCODEC_AMBE  (D-STAR;           zorcom-codec-ambe)
 *                                    -DCODEC_IMBE  (P25 phase 1;      zorcom-codec-imbe)
 * Build (macOS / Linux), with mbelib's header and library installed:
 *   cc -shared -fPIC -O2 -DCODEC_AMBE2 mbelib_wrapper.c -lmbe -o zorcom-codec-ambe2.dylib      (use .so on Linux)
 * Windows (MSYS2): cc -shared -O2 -DCODEC_AMBE2 mbelib_wrapper.c -lmbe -o zorcom-codec-ambe2.dll
 * Copy the result into the Eye of ZorCom codecs folder and press "Rescan for plug-in".
 *
 * mbelib API used (mbelib.h 1.3): mbe_initMbeParms, mbe_processAmbe3600x2450Frame, mbe_processAmbe3600x2400Frame, mbe_processImbe7200x4400Frame.
 * Their argument lists differ between mbelib forks; adjust the call lines to your version if the compiler complains. */
#include <mbelib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

#if defined(CODEC_IMBE)
#define ROWS 8
#define COLS 23
#else
#define ROWS 4
#define COLS 24
#endif

typedef struct { mbe_parms cur, prev, prevEnh; } State;

API void* zorcom_speech_open(void) {
    State* s = (State*)calloc(1, sizeof *s);
    if (s) mbe_initMbeParms(&s->cur, &s->prev, &s->prevEnh);
    return s;
}

API void zorcom_speech_close(void* state) { free(state); }

API int zorcom_speech_sample_rate(void) { return 8000; }

/* coded: the ROWS x COLS code-word bits, row by row, most significant bit first (12 bytes for AMBE / AMBE+2, 23 for IMBE) */
API int zorcom_speech_decode(void* state, const uint8_t* coded, size_t nbytes, int16_t* pcm, size_t max_samples) {
    State* s = (State*)state;
    char fr[ROWS][COLS];
    char data[88];
    char err[64];
    int errs = 0, errs2 = 0;
    short out[160];
    if (!s || max_samples < 160 || nbytes < (size_t)((ROWS * COLS + 7) / 8)) return -1;
    for (int i = 0; i < ROWS * COLS; i++) fr[i / COLS][i % COLS] = (coded[i / 8] >> (7 - (i % 8))) & 1;
    memset(data, 0, sizeof data);
    memset(err, 0, sizeof err);
#if defined(CODEC_AMBE2)
    mbe_processAmbe3600x2450Frame(out, &errs, &errs2, err, fr, data, &s->cur, &s->prev, &s->prevEnh, 3);
#elif defined(CODEC_AMBE)
    mbe_processAmbe3600x2400Frame(out, &errs, &errs2, err, fr, data, &s->cur, &s->prev, &s->prevEnh, 3);
#elif defined(CODEC_IMBE)
    mbe_processImbe7200x4400Frame(out, &errs, &errs2, err, fr, data, &s->cur, &s->prev, &s->prevEnh, 3);
#else
#error "define CODEC_AMBE2, CODEC_AMBE or CODEC_IMBE"
#endif
    for (int i = 0; i < 160; i++) pcm[i] = out[i];
    return 160;
}
