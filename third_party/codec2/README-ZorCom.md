# codec2 in Eye of ZorCom

Eye of ZorCom ships **codec2 1.2.0** (https://github.com/drowe67/codec2, by David Rowe and contributors) for the speech of FreeDV and M17.

- **Licence:** GNU Lesser General Public License, version 2.1 (the text is in `COPYING`, shipped with every Eye of ZorCom package as `licenses/codec2/COPYING`).
- **Separate library:** codec2 is **not linked into Eye of ZorCom**. It is built as its own shared library (`libcodec2.so.1.2`, `libcodec2.1.2.dylib`,
  `libcodec2.dll`) that Eye of ZorCom loads at run time with `dlopen` / `LoadLibrary` (`core/src/m17/m17_codec2.cpp`, `core/src/hfdig/hfdig_freedv.cpp`).
- **You can replace it:** put your own build of codec2 (any 1.x with the same functions) in Eye of ZorCom's `codecs` folder, or set `DECT2_CODEC2_PATH`
  to its full path; Eye of ZorCom looks there before the copy it ships (docs/CODECS.md).
- **Source:** the files in `src/` are the unmodified sources of the 1.2.0 release (only the command-line tools and test programs are left out).
  `src/codebook*.c` are the output of the release's own `generate_codebook` tool (`src/generate_codebook.c`), made once from the release's
  `src/codebook/*.txt`, because upstream makes them while it builds, which does not work when cross-compiling for Windows. `CMakeLists.txt` and
  `include/codec2/version.h` (made from `cmake/version.h.in`) are Eye of ZorCom's own and replace upstream's build files.
- **Build it yourself:** `cmake -S . -B build && cmake --build build --target zorcom_codec2` makes the library in `build/`.
  `-DDECT2_BUNDLE_CODEC2=OFF` leaves it out.

codec2 is a codec for speech at low bit rates and is free software; it is not patent-encumbered like AMBE.
