# RNNoise in Eye of SaurCom

Neural noise suppression for speech (recurrent network + band-gain filter), used by the Listen
"Noise remover (neural)" option.

- Upstream: https://github.com/xiph/rnnoise (Xiph.Org / Mozilla, Jean-Marc Valin et al.)
- Version: tag `v0.1.1`, commit `6cbfd53eb348a8d394e0757b4025c6ded34eb2b6`
- Licence: BSD-3-Clause, see `COPYING` (unchanged), authors in `AUTHORS`.
- Model: the small built-in model of that tag, `src/rnn_data.c` (about 425 KB, 48 kHz, 10 ms frames).
  The newer v0.2 model data is 15-29 MB of source and is not vendored.

Vendored: `src/*.c|h` needed for the library and `include/rnnoise.h`. Left out: `rnn_reader.c`, `rnn_train.py`,
`compile.sh` (not used by the build), plus the training, examples, autotools and docs folders.

## Local changes (src/denoise.c only)

1. `CommonState common` is `static` (was an exported global named `common`).
2. `rnnoise_init()` calls `check_init()` so the shared FFT / window / DCT tables are built when a state is
   created and never lazily on the audio thread. Eye of SaurCom serialises state creation with a mutex.

Built as the static library `dect2_rnnoise` (plain C99, without -ffast-math, which upstream rejects).
