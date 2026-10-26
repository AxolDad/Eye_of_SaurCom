# A starting point for your own Eye of ZorCom plugin. Copy this folder, rename "my-plugin" everywhere (also in manifest.json, whose "id" must equal it),
# put the path of Eye of ZorCom's sdk/ folder in ZORCOM_SDK_DIR and build:
#   cmake -S . -B build -DZORCOM_SDK_DIR=/path/to/ZorCom/sdk && cmake --build build
# The result is build/plugins/my-plugin/ (library + manifest.json): copy that folder into Eye of ZorCom's plugins folder (View > Plugins > Open folder).
cmake_minimum_required(VERSION 3.16)
project(my_plugin CXX)
set(CMAKE_CXX_STANDARD 17)
set(ZORCOM_SDK_DIR "" CACHE PATH "Eye of ZorCom's sdk/ folder")
include(${ZORCOM_SDK_DIR}/cmake/ZorComPlugin.cmake)
zorcom_add_plugin(my-plugin SOURCES plugin.cpp INCLUDE ${ZORCOM_SDK_DIR}/include)
