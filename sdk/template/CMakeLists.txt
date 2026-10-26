# A starting point for your own Eye of SaurCom plugin. Copy this folder, rename "my-plugin" everywhere (also in manifest.json, whose "id" must equal it),
# put the path of Eye of SaurCom's sdk/ folder in SAURCOM_SDK_DIR and build:
#   cmake -S . -B build -DSAURCOM_SDK_DIR=/path/to/SaurCom/sdk && cmake --build build
# The result is build/plugins/my-plugin/ (library + manifest.json): copy that folder into Eye of SaurCom's plugins folder (View > Plugins > Open folder).
cmake_minimum_required(VERSION 3.16)
project(my_plugin CXX)
set(CMAKE_CXX_STANDARD 17)
set(SAURCOM_SDK_DIR "" CACHE PATH "Eye of SaurCom's sdk/ folder")
include(${SAURCOM_SDK_DIR}/cmake/SaurComPlugin.cmake)
saurcom_add_plugin(my-plugin SOURCES plugin.cpp INCLUDE ${SAURCOM_SDK_DIR}/include)
