# ESP-IDF host-side unit tests (Unity framework)
# Run with: idf.py -C test build && idf.py -C test flash monitor

cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voicemask_firmware_test)

# Unity test runner
idf_component_register(
    SRCS "test_psm_encode.c"
    INCLUDE_DIRS "."
    REQUIRES unity
)
