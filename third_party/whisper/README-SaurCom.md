# whisper.cpp / ggml in Eye of SaurCom

Speech to text for the live transcripts (`core/src/transcribe`, `docs/TRANSCRIBE.md`).

- Upstream: https://github.com/ggml-org/whisper.cpp (Georgi Gerganov and the ggml authors)
- Version: release `v1.9.5` (tag object `d2a764c648779feb36d44363f212a29b00aad1af`), commit `d1be6fde11ac6e0407606b4e42fe72d34add8037`
- Licence: MIT, see `LICENSE` (unchanged; it is also `LICENSES/MIT.txt`). It is installed under `doc/whisper` with this file.
- Vendored: `include/whisper.h`, `src/whisper.cpp`, `src/whisper-arch.h`, and `ggml/` (headers, `src/` core, the CPU back end `ggml-cpu`, the CMake files).
- Left out: every other ggml back end (CUDA, Metal, Vulkan, OpenCL, SYCL, BLAS, CANN, ...), KleidiAI and SpacemiT in the CPU back end, the
  CoreML / OpenVINO / VitisAI encoders, the examples, tests, bindings, models and samples. Eye of SaurCom builds it as a static library (`dect2_whisper`,
  CMake option `DECT2_WITH_WHISPER`), CPU only, OpenMP off, with Accelerate on macOS. Not built for Android, iOS or WebAssembly.
- Other copyright notices inside the vendored files (all MIT, kept in the file headers): `ggml/src/ggml-cpu/llamafile/sgemm.cpp` (Mozilla Foundation, 2024), and the YaRN
  routines in `ggml/src/ggml-cpu/ops.cpp` (Jeffrey Quesnelle and Bowen Peng, 2023).
- **No model is included.** The program downloads one on the user's explicit click (SHA-256 checked) or reads a file the user puts in the models folder.

## Local changes

1. `src/aneforge/whisper-aneforge.cpp` is replaced by empty stubs (upstream loads a library with `dlopen` for an experimental Neural Engine encoder; that
   is not wanted here and does not exist on Windows). `whisper-aneforge.h` is unchanged.
2. Nothing else in the vendored files is edited. `WHISPER_VERSION` is defined by `CMakeLists.txt`.
