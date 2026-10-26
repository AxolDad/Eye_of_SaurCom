# Optional add-on: the DRA+ decoder for CDR sound, built as libsaurcom_dra (Eye of SaurCom loads it at run time, docs/CODECS.md "DRA+ (CDR sound)").
# Not part of the Eye of SaurCom build, never installed or packaged. The decoder source is NOT in this repository: get it with extract_dra.py.
#   python3 extract_dra.py                  (downloads maliwen2015's patch, writes dra_src/dradec.c and dradec.h)
#   cmake -S . -B build -DDRA_SRC=dra_src && cmake --build build
# then copy the library (libsaurcom_dra.dylib / libsaurcom_dra.so / saurcom_dra.dll) into the Eye of SaurCom codecs folder.
cmake_minimum_required(VERSION 3.16)
project(saurcom_dra C)
set(DRA_SRC "${CMAKE_CURRENT_SOURCE_DIR}/dra_src" CACHE PATH "folder with dradec.c and dradec.h (extract_dra.py)")
if(NOT EXISTS "${DRA_SRC}/dradec.c" OR NOT EXISTS "${DRA_SRC}/dradec.h")
  message(FATAL_ERROR "dradec.c / dradec.h not found in ${DRA_SRC}: run  python3 extract_dra.py  first (see the comment at the top of this file)")
endif()
add_library(saurcom_dra SHARED "${DRA_SRC}/dradec.c")
target_include_directories(saurcom_dra PRIVATE "${DRA_SRC}")
if(NOT MSVC)
  target_compile_options(saurcom_dra PRIVATE -w -O2)
  target_link_libraries(saurcom_dra PRIVATE m)
endif()
set_target_properties(saurcom_dra PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON C_VISIBILITY_PRESET default)
if(WIN32)
  set_target_properties(saurcom_dra PROPERTIES PREFIX "")   # saurcom_dra.dll
endif()
